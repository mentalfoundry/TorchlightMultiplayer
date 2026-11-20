#include "net/tailcat.h"
#include "log.h"

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12) // mstcpip.h, behind a Windows-version guard
#endif

namespace tlmp::net::tailcat {

namespace {

// --- libtailcat.dll (cmd/libtailcat/tailcat.h, ABI 1) ------------------------------------------------
typedef uint64_t tc_handle;
constexpr tc_handle kNoCancel = 0;
constexpr int32_t kOk = 0;
constexpr int32_t kUdp = 2;
constexpr size_t kMaxDatagram = 1232;

struct Api
{
  uint32_t (*abiVersion)() = nullptr;
  void (*free)(void*) = nullptr;
  int32_t (*tokenNew)(int64_t, tc_handle*, char**) = nullptr;
  int32_t (*close)(tc_handle, char**) = nullptr;
  int32_t (*clientNew)(char*, tc_handle*, char**) = nullptr;
  int32_t (*serverNew)(char*, tc_handle*, char**) = nullptr;
  int32_t (*clientDial)(tc_handle, tc_handle, uint16_t, int32_t, tc_handle*, char**) = nullptr;
  int32_t (*serverListen)(tc_handle, tc_handle, uint16_t, int32_t, tc_handle*, char**) = nullptr;
  int32_t (*listenerAccept)(tc_handle, tc_handle, tc_handle*, char**) = nullptr;
  int32_t (*connRead)(tc_handle, tc_handle, void*, size_t, size_t*, char**) = nullptr;
  int32_t (*connWrite)(tc_handle, tc_handle, void*, size_t, size_t*, char**) = nullptr;
  int32_t (*info)(tc_handle, tc_handle, char**, char**) = nullptr;
  bool ok = false;
};
Api g_api;

bool load()
{
  static std::once_flag once;
  std::call_once(once, [] {
    // Next to Torchlight.exe (the game's folder), whatever the current directory.
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != L'\\')
      n--;
    wcscpy_s(path + n, MAX_PATH - n, L"libtailcat.dll");
    HMODULE m = LoadLibraryW(path);
    if (!m)
    {
      TLMP_LOG("tailcat: libtailcat.dll not loaded (error %lu); no join codes", GetLastError());
      return;
    }
    auto get = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
      return fn != nullptr;
    };
    bool all = get(g_api.abiVersion, "tc_abi_version") && get(g_api.free, "tc_free") &&
               get(g_api.tokenNew, "tc_token_new") && get(g_api.close, "tc_close") &&
               get(g_api.clientNew, "tc_client_new") && get(g_api.serverNew, "tc_server_new") &&
               get(g_api.clientDial, "tc_client_dial") && get(g_api.serverListen, "tc_server_listen") &&
               get(g_api.listenerAccept, "tc_listener_accept") && get(g_api.connRead, "tc_conn_read") &&
               get(g_api.connWrite, "tc_conn_write") && get(g_api.info, "tc_info");
    if (!all || g_api.abiVersion() != 1)
    {
      TLMP_LOG("tailcat: libtailcat.dll is not the expected version (ABI %u); no join codes",
               all ? g_api.abiVersion() : 0u);
      return;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa); // our relay sockets (ENet does its own)
    g_api.ok = true;
  });
  return g_api.ok;
}

std::string message(char* error)
{
  std::string s = error ? error : "";
  if (error)
    g_api.free(error);
  return s;
}

// --- state ---------------------------------------------------------------------------------------------
std::mutex g_lock;
State g_state = State::Off;
std::string g_error;
std::string g_code;
uint16_t g_localPort = 0;
tc_handle g_peer = 0; // the server or client: closing it ends every call and flow under it
std::vector<std::thread> g_threads;
std::vector<SOCKET> g_sockets;
std::atomic<bool> g_stopping{false};
std::atomic<unsigned> g_flows{0};
std::atomic<bool> g_enabled{true};

void set(State s, const std::string& error = {})
{
  std::lock_guard<std::mutex> l(g_lock);
  g_state = s;
  g_error = error;
}

void spawn(std::thread t)
{
  std::lock_guard<std::mutex> l(g_lock);
  g_threads.push_back(std::move(t));
}

SOCKET relaySocket()
{
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET)
    return s;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
  {
    closesocket(s);
    return INVALID_SOCKET;
  }
  // A datagram to a closed port comes back as an error on the next recv (ICMP); ignore those.
  BOOL off = FALSE;
  DWORD unused = 0;
  WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &unused, nullptr, nullptr);
  std::lock_guard<std::mutex> l(g_lock);
  g_sockets.push_back(s);
  return s;
}

// Closes a relay socket unless stop() already took it (whoever removes it from the list closes it).
void releaseSocket(SOCKET s)
{
  {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = std::find(g_sockets.begin(), g_sockets.end(), s);
    if (it == g_sockets.end())
      return;
    g_sockets.erase(it);
  }
  shutdown(s, SD_BOTH);
  closesocket(s);
}

uint16_t portOf(SOCKET s)
{
  sockaddr_in a{};
  int len = sizeof(a);
  return getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0 ? ntohs(a.sin_port) : 0;
}

// Flow -> local socket. `to` is where ENet listens (host) or, on a client, ENet's own socket once it has
// sent something (until then datagrams from the host can't be delivered and are dropped).
void flowToSocket(tc_handle flow, SOCKET s, const sockaddr_in* to, const std::atomic<bool>* known)
{
  char buf[kMaxDatagram + 16];
  for (;;)
  {
    size_t n = 0;
    char* e = nullptr;
    if (g_api.connRead(flow, kNoCancel, buf, sizeof(buf), &n, &e) != kOk)
    {
      message(e);
      break;
    }
    if (known && !known->load())
      continue;
    sendto(s, buf, static_cast<int>(n), 0, reinterpret_cast<const sockaddr*>(to), sizeof(*to));
  }
}

// Local socket -> flow. Remembers who sent (a client's ENet) in *from.
void socketToFlow(SOCKET s, tc_handle flow, sockaddr_in* from, std::atomic<bool>* known)
{
  char buf[2048];
  for (;;)
  {
    sockaddr_in src{};
    int len = sizeof(src);
    int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&src), &len);
    if (n < 0)
    {
      if (g_stopping)
        break;
      continue; // a stray ICMP report
    }
    if (from && !known->load())
    {
      *from = src;
      known->store(true);
    }
    if (static_cast<size_t>(n) > kMaxDatagram)
    {
      TLMP_LOG("tailcat: dropped a %d-byte datagram (more than %zu)", n, kMaxDatagram);
      continue;
    }
    size_t w = 0;
    char* e = nullptr;
    if (g_api.connWrite(flow, kNoCancel, buf, static_cast<size_t>(n), &w, &e) != kOk)
    {
      message(e);
      break;
    }
  }
}

// Host: one joining client's flow, relayed to the ENet port as if it came from 127.0.0.1:<port>.
void serveFlow(tc_handle flow, uint16_t enetPort)
{
  SOCKET s = relaySocket();
  if (s == INVALID_SOCKET)
  {
    g_api.close(flow, nullptr);
    return;
  }
  unsigned n = ++g_flows;
  TLMP_LOG("tailcat: a player joined through the code (flow %u, relayed from port %u)", n, portOf(s));
  sockaddr_in enet{};
  enet.sin_family = AF_INET;
  enet.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  enet.sin_port = htons(enetPort);
  std::thread back([s, flow] { socketToFlow(s, flow, nullptr, nullptr); });
  flowToSocket(flow, s, &enet, nullptr);
  // The flow ended (idle, or the session is stopping): let the other direction go too.
  g_api.close(flow, nullptr);
  releaseSocket(s);
  back.join();
  TLMP_LOG("tailcat: flow %u ended", n);
}

void hostMain(uint16_t enetPort)
{
  char* e = nullptr;
  tc_handle server = 0, listener = 0;
  char config[] = "{}";
  if (g_api.serverNew(config, &server, &e) != kOk)
  {
    set(State::Failed, "could not create the Tailcat server: " + message(e));
    return;
  }
  {
    std::lock_guard<std::mutex> l(g_lock);
    g_peer = server;
  }
  DWORD t0 = GetTickCount();
  if (g_api.serverListen(server, kNoCancel, kServicePort, kUdp, &listener, &e) != kOk)
  {
    set(State::Failed, "could not start Tailcat: " + message(e));
    return;
  }
  char* json = nullptr;
  if (g_api.info(server, kNoCancel, &json, &e) != kOk)
  {
    set(State::Failed, "no Tailcat address: " + message(e));
    return;
  }
  std::string info = json;
  g_api.free(json);
  size_t a = info.find("\"address\":\"");
  size_t end = a == std::string::npos ? a : info.find('"', a + 11);
  if (end == std::string::npos)
  {
    set(State::Failed, "no address in Tailcat's server info");
    return;
  }
  {
    std::lock_guard<std::mutex> l(g_lock);
    g_code = info.substr(a + 11, end - a - 11);
    g_state = State::Ready;
  }
  TLMP_LOG("tailcat: join code ready in %.1f s (%zu characters)", (GetTickCount() - t0) / 1000.0, g_code.size());
  for (;;)
  {
    tc_handle flow = 0;
    if (g_api.listenerAccept(listener, kNoCancel, &flow, &e) != kOk)
    {
      message(e);
      break; // the server was closed
    }
    spawn(std::thread([flow, enetPort] { serveFlow(flow, enetPort); }));
  }
}

void clientMain(std::string code, SOCKET s)
{
  char* e = nullptr;
  tc_handle client = 0, flow = 0, token = 0;
  std::string config = "{\"address\":\"" + code + "\"}";
  if (g_api.clientNew(config.data(), &client, &e) != kOk)
  {
    set(State::Failed, "that join code was not accepted: " + message(e));
    return;
  }
  {
    std::lock_guard<std::mutex> l(g_lock);
    g_peer = client;
  }
  DWORD t0 = GetTickCount();
  g_api.tokenNew(45'000'000'000LL, &token, nullptr); // 45 s to reach the host
  int32_t status = g_api.clientDial(client, token, kServicePort, kUdp, &flow, &e);
  g_api.close(token, nullptr);
  if (status != kOk)
  {
    std::string why = message(e);
    set(State::Failed, status == 3 ? "the host did not answer (is the code still valid?)" : "could not connect: " + why);
    return;
  }
  TLMP_LOG("tailcat: reached the host in %.1f s", (GetTickCount() - t0) / 1000.0);
  set(State::Ready);
  sockaddr_in enet{};
  std::atomic<bool> known{false};
  std::thread up([s, flow, &enet, &known] { socketToFlow(s, flow, &enet, &known); });
  flowToSocket(flow, s, &enet, &known);
  releaseSocket(s);
  up.join();
  if (!g_stopping)
    TLMP_LOG("tailcat: the connection through the code ended");
}

} // namespace

bool available()
{
  return load();
}

void setEnabled(bool on)
{
  g_enabled = on;
}

bool enabled()
{
  return g_enabled;
}

State state()
{
  std::lock_guard<std::mutex> l(g_lock);
  return g_state;
}

std::string error()
{
  std::lock_guard<std::mutex> l(g_lock);
  return g_error;
}

void startHost(uint16_t enetPort)
{
  stop();
  if (!load())
  {
    set(State::Failed, "libtailcat.dll is missing");
    return;
  }
  g_stopping = false;
  set(State::Starting);
  TLMP_LOG("tailcat: getting a join code");
  spawn(std::thread(hostMain, enetPort));
}

std::string code()
{
  std::lock_guard<std::mutex> l(g_lock);
  return g_state == State::Ready ? g_code : std::string();
}

void startClient(const std::string& joinCode)
{
  stop();
  if (!load())
  {
    set(State::Failed, "libtailcat.dll is missing");
    return;
  }
  if (!looksLikeCode(joinCode))
  {
    set(State::Failed, "that is not a join code");
    return;
  }
  g_stopping = false;
  SOCKET s = relaySocket();
  if (s == INVALID_SOCKET)
  {
    set(State::Failed, "no local port for the connection");
    return;
  }
  {
    std::lock_guard<std::mutex> l(g_lock);
    g_localPort = portOf(s);
  }
  set(State::Starting);
  TLMP_LOG("tailcat: connecting with a join code (local port %u)", g_localPort);
  spawn(std::thread(clientMain, joinCode, s));
}

uint16_t localPort()
{
  std::lock_guard<std::mutex> l(g_lock);
  return g_localPort;
}

void stop()
{
  tc_handle peer;
  std::vector<std::thread> threads;
  std::vector<SOCKET> sockets;
  {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_state == State::Off && g_threads.empty())
      return;
    peer = g_peer;
    g_peer = 0;
    sockets.swap(g_sockets);
  }
  g_stopping = true;
  // Closing the server/client interrupts every call under it (accept, reads, writes); closing the
  // sockets ends the relays' receives. Then every thread can finish.
  if (peer)
    g_api.close(peer, nullptr);
  for (SOCKET s : sockets)
  {
    shutdown(s, SD_BOTH);
    closesocket(s);
  }
  {
    std::lock_guard<std::mutex> l(g_lock);
    threads.swap(g_threads);
  }
  for (std::thread& t : threads)
    if (t.joinable())
      t.join();
  std::lock_guard<std::mutex> l(g_lock);
  g_state = State::Off;
  g_error.clear();
  g_code.clear();
  g_localPort = 0;
  TLMP_LOG("tailcat: stopped");
}

bool looksLikeCode(const std::string& text)
{
  // Tailcat addresses are long and base64url (letters, digits, '-', '_'); IP addresses and host
  // names are short and have dots or colons.
  if (text.size() < 100 || text.size() > 1000)
    return false;
  for (char c : text)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  return true;
}

} // namespace tlmp::net::tailcat
