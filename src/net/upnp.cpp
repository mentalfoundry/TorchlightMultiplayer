#include "net/upnp.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <natupnp.h>
#include <oleauto.h>
#include <winhttp.h>

#include <mutex>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

namespace tlmp::net::upnp {

static std::mutex g_lock;
static Status g_status;
static uint16_t g_mappedPort = 0;
static std::thread g_worker;

static void set(State s, const std::string& detail, const std::string& ip = {})
{
  std::lock_guard<std::mutex> l(g_lock);
  g_status.state = s;
  g_status.detail = detail;
  g_status.externalIp = ip;
}

Status status()
{
  std::lock_guard<std::mutex> l(g_lock);
  return g_status;
}

// The address this machine reaches the internet from (the one the router forwards to): the
// local end of a UDP "connection" toward a public address (nothing is sent).
static std::wstring localAddress()
{
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    return {};
  std::wstring out;
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s != INVALID_SOCKET)
  {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    sockaddr_in me{};
    int len = sizeof(me);
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0)
    {
      wchar_t buf[64] = {};
      InetNtopW(AF_INET, &me.sin_addr, buf, 64);
      out = buf;
    }
    closesocket(s);
  }
  WSACleanup();
  return out;
}

static IStaticPortMappingCollection* mappings(IUPnPNAT** nat)
{
  *nat = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(nat))) ||
      !*nat)
    return nullptr;
  IStaticPortMappingCollection* list = nullptr;
  if (FAILED((*nat)->get_StaticPortMappingCollection(&list)))
    list = nullptr;
  return list; // null: no UPnP router (or UPnP off)
}

static std::string narrow(const wchar_t* w)
{
  std::string s;
  for (; w && *w; ++w)
    s += static_cast<char>(*w < 128 ? *w : '?');
  return s;
}

static void work(uint16_t port)
{
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IUPnPNAT* nat = nullptr;
  IStaticPortMappingCollection* list = mappings(&nat);
  if (!list)
  {
    set(State::NoRouter, "no UPnP router answered");
  }
  else
  {
    std::wstring local = localAddress();
    BSTR proto = SysAllocString(L"UDP");
    BSTR client = SysAllocString(local.c_str());
    BSTR desc = SysAllocString(L"Torchlight co-op (TLMP)");
    IStaticPortMapping* mapping = nullptr;
    HRESULT hr = list->Add(port, proto, port, client, VARIANT_TRUE, desc, &mapping);
    if (SUCCEEDED(hr) && mapping)
    {
      BSTR ext = nullptr;
      std::string ip;
      if (SUCCEEDED(mapping->get_ExternalIPAddress(&ext)) && ext)
      {
        ip = narrow(ext);
        SysFreeString(ext);
      }
      mapping->Release();
      {
        std::lock_guard<std::mutex> l(g_lock);
        g_mappedPort = port;
      }
      set(State::Open, "mapped UDP " + std::to_string(port) + " to " + narrow(local.c_str()), ip);
    }
    else
    {
      char b[64];
      std::snprintf(b, sizeof(b), "the router refused the mapping (0x%08lx)", static_cast<unsigned long>(hr));
      set(State::Failed, b);
    }
    SysFreeString(proto);
    SysFreeString(client);
    SysFreeString(desc);
    list->Release();
  }
  if (nat)
    nat->Release();
  CoUninitialize();
  Status s = status();
  TLMP_LOG("upnp: %s%s%s", s.detail.c_str(), s.externalIp.empty() ? "" : "; external address ", s.externalIp.c_str());
}

void open(uint16_t port)
{
  if (g_worker.joinable())
    return; // once per run
  set(State::Working, "asking the router");
  g_worker = std::thread(work, port);
}

void close()
{
  if (g_worker.joinable())
    g_worker.join();
  uint16_t port;
  {
    std::lock_guard<std::mutex> l(g_lock);
    port = g_mappedPort;
    g_mappedPort = 0;
  }
  if (!port)
    return;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IUPnPNAT* nat = nullptr;
  if (IStaticPortMappingCollection* list = mappings(&nat))
  {
    BSTR proto = SysAllocString(L"UDP");
    HRESULT hr = list->Remove(port, proto);
    SysFreeString(proto);
    list->Release();
    TLMP_LOG("upnp: removed the UDP %u mapping (0x%08lx)", port, static_cast<unsigned long>(hr));
  }
  if (nat)
    nat->Release();
  CoUninitialize();
  set(State::Idle, "closed");
}

std::string localAddressText()
{
  static std::string cached;
  if (cached.empty())
  {
    std::wstring w = localAddress();
    cached = narrow(w.c_str());
  }
  return cached;
}

static std::mutex g_publicLock;
static std::string g_public;
static bool g_lookupStarted = false;

std::string publicAddress()
{
  {
    Status s = status();
    if (s.state == State::Open && !s.externalIp.empty())
      return s.externalIp; // what the router says
  }
  std::lock_guard<std::mutex> l(g_publicLock);
  return g_public;
}

// One HTTPS GET to api.ipify.org, which answers with the caller's public IP as plain text.
static void lookup()
{
  std::string ip;
  HINTERNET session = WinHttpOpen(L"TLMP", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connect = session ? WinHttpConnect(session, L"api.ipify.org", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                              : nullptr;
  if (request)
    WinHttpSetTimeouts(request, 5000, 5000, 5000, 5000);
  if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr))
  {
    char buf[64] = {};
    DWORD got = 0;
    if (WinHttpReadData(request, buf, sizeof(buf) - 1, &got))
    {
      buf[got] = 0;
      for (char* c = buf; *c; ++c)
        if ((*c >= '0' && *c <= '9') || *c == '.' || *c == ':' || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F'))
          ip += *c;
        else
          break;
    }
  }
  if (request)
    WinHttpCloseHandle(request);
  if (connect)
    WinHttpCloseHandle(connect);
  if (session)
    WinHttpCloseHandle(session);
  TLMP_LOG("upnp: public address %s", ip.empty() ? "unknown (lookup failed)" : ip.c_str());
  std::lock_guard<std::mutex> l(g_publicLock);
  g_public = ip;
}

void setPublicAddressForTest(const std::string& address)
{
  std::lock_guard<std::mutex> l(g_publicLock);
  g_public = address;
}

void lookupPublicAddress()
{
  if (g_lookupStarted)
    return; // once per run
  g_lookupStarted = true;
  std::thread(lookup).detach();
}

bool probe(std::string* detail)
{
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IUPnPNAT* nat = nullptr;
  IStaticPortMappingCollection* list = mappings(&nat);
  long count = -1;
  if (list)
  {
    list->get_Count(&count);
    list->Release();
  }
  if (nat)
    nat->Release();
  CoUninitialize();
  if (detail)
    *detail = list ? "a UPnP router answered (" + std::to_string(count) + " mappings)" : "no UPnP router answered";
  return list != nullptr;
}

} // namespace tlmp::net::upnp
