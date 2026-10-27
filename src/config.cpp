#include "config.h"
#include "log.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "shell32.lib")

namespace tlmp {

static std::string narrow(const wchar_t* w)
{
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1)
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

static void parseHostPort(const std::string& s, Config& cfg)
{
  size_t colon = s.rfind(':');
  if (colon == std::string::npos)
  {
    cfg.address = s;
    return;
  }
  cfg.address = s.substr(0, colon);
  cfg.port = static_cast<uint16_t>(std::atoi(s.c_str() + colon + 1));
}

Config loadConfig()
{
  Config cfg;

  // Relative path resolves against the game directory; Torchlight sets the CWD to
  // its install folder on startup (SetCurrentDirectoryW import).
  const char* ini = ".\\TLMP.ini";
  char buf[256];

  GetPrivateProfileStringA("net", "mode", "off", buf, sizeof(buf), ini);
  if (_stricmp(buf, "host") == 0)
    cfg.mode = NetMode::Host;
  else if (_stricmp(buf, "client") == 0 || _stricmp(buf, "join") == 0)
    cfg.mode = NetMode::Client;

  GetPrivateProfileStringA("net", "address", cfg.address.c_str(), buf, sizeof(buf), ini);
  cfg.address = buf;
  cfg.port = static_cast<uint16_t>(GetPrivateProfileIntA("net", "port", cfg.port, ini));
  GetPrivateProfileStringA("player", "name", cfg.playerName.c_str(), buf, sizeof(buf), ini);
  cfg.playerName = buf;

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; i++)
  {
    if (_wcsicmp(argv[i], L"-tlmp-host") == 0)
    {
      cfg.mode = NetMode::Host;
      if (i + 1 < argc && iswdigit(argv[i + 1][0]))
        cfg.port = static_cast<uint16_t>(_wtoi(argv[++i]));
    }
    else if (_wcsicmp(argv[i], L"-tlmp-join") == 0 && i + 1 < argc)
    {
      cfg.mode = NetMode::Client;
      parseHostPort(narrow(argv[++i]), cfg);
    }
    else if (_wcsicmp(argv[i], L"-tlmp-worldseed") == 0 && i + 1 < argc)
    {
      cfg.devWorldSeed = static_cast<uint32_t>(_wcstoui64(argv[++i], nullptr, 10));
    }
    else if (_wcsicmp(argv[i], L"-tlmp-repairpet") == 0 && i + 2 < argc)
    {
      cfg.repairPetType = argv[++i];
      cfg.repairPetName = argv[++i];
    }
    else if (_wcsicmp(argv[i], L"-tlmp-test") == 0 && i + 1 < argc)
    {
      cfg.testScript = argv[++i];
    }
    else if (_wcsicmp(argv[i], L"-tlmp-mirror") == 0)
    {
      cfg.devMirror = true;
    }
    else if (_wcsicmp(argv[i], L"-tlmp-name") == 0 && i + 1 < argc)
    {
      cfg.playerName = narrow(argv[++i]);
    }
  }
  LocalFree(argv);

  if (cfg.playerName.size() > 31)
    cfg.playerName.resize(31);

  return cfg;
}

const char* toString(NetMode mode)
{
  switch (mode)
  {
    case NetMode::Host:
      return "host";
    case NetMode::Client:
      return "client";
    default:
      return "off";
  }
}

} // namespace tlmp
