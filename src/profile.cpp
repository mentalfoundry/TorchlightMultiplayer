// Per-instance profile redirection, used to run several Torchlight instances from one
// install (local multiplayer testing) without them sharing settings and save files.
//
//   Torchlight.exe -tlmp-profile p2 -tlmp-join 127.0.0.1
//
// Torchlight builds most user-data paths through 0x5CC6C0:
//   SHGetSpecialFolderPathW(0, buf, CSIDL_APPDATA, TRUE) + "/Runic Games/Torchlight/"
// and calls it fresh each time (19 call sites). The only import of
// SHGetSpecialFolderPathW is through the IAT slot below, so patching that slot redirects
// all of them. Directory creation (0x5CCA70) uses %APPDATA% instead, which is
// overridden for the process as well. WinMain reads settings before Ogre loads plugins, so this runs from
// DllMain: the OgreGUIRenderer.dll proxy makes TLMP.dll a load-time dependency.
// Only kernel32 is used here, which is safe under the loader lock.

#include "profile.h"
#include "game/game.h"
#include "net/session.h"

#include <windows.h>

#include <cwchar>

namespace tlmp::profile {

// IAT slot for SHELL32!SHGetSpecialFolderPathW in Torchlight.exe (GOG 1.7.8.70).
static auto* const kIatSHGetSpecialFolderPathW = reinterpret_cast<void**>(0x00A86398);
constexpr int kCsidlAppData = 0x1A;

using SHGetSpecialFolderPathWFn = BOOL(WINAPI*)(HWND, LPWSTR, int, BOOL);
static SHGetSpecialFolderPathWFn g_original = nullptr;
static wchar_t g_profileDir[MAX_PATH];

static BOOL WINAPI hookedSHGetSpecialFolderPathW(HWND hwnd, LPWSTR path, int csidl, BOOL create)
{
  if ((csidl & 0xFF) == kCsidlAppData)
  {
    if (create)
      CreateDirectoryW(g_profileDir, nullptr);
    wcscpy_s(path, MAX_PATH, g_profileDir);
    return TRUE;
  }
  return g_original(hwnd, path, csidl, create);
}

// Extracts the token after "-tlmp-profile". Accepts [A-Za-z0-9_-] only, so the name
// can't escape the profiles directory.
static bool profileFromCommandLine(wchar_t* out, size_t outLen)
{
  const wchar_t* cmd = GetCommandLineW();
  const wchar_t* key = L"-tlmp-profile";
  const wchar_t* p = wcsstr(cmd, key);
  if (!p)
    return false;
  p += wcslen(key);
  while (*p == L' ' || *p == L'\t' || *p == L'"')
    p++;
  size_t n = 0;
  while (n + 1 < outLen && (iswalnum(*p) || *p == L'_' || *p == L'-'))
    out[n++] = *p++;
  out[n] = 0;
  return n > 0;
}

// New profiles start from a copy of the real profile's settings so display mode,
// key bindings etc. carry over. Saves are intentionally not copied.
static void seedSettings()
{
  wchar_t appData[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return;

  wchar_t dst[MAX_PATH];
  swprintf_s(dst, L"%s\\Runic Games", g_profileDir);
  CreateDirectoryW(dst, nullptr);
  wcscat_s(dst, L"\\Torchlight");
  CreateDirectoryW(dst, nullptr);

  for (const wchar_t* file : {L"settings.txt", L"local_settings.txt"})
  {
    wchar_t from[MAX_PATH], to[MAX_PATH];
    swprintf_s(from, L"%s\\Runic Games\\Torchlight\\%s", appData, file);
    swprintf_s(to, L"%s\\%s", dst, file);
    CopyFileW(from, to, TRUE); // TRUE: never overwrite an existing profile's settings
  }
}

void installEarly()
{
  wchar_t name[64];
  if (!profileFromCommandLine(name, ARRAYSIZE(name)))
    return;
  if (net::gameBuildStamp() != game::kExpectedBuildStamp)
    return; // the IAT address is only valid for the build we reverse engineered

  // <game dir>\tlmp_profiles\<name>; the game appends "/Runic Games/Torchlight/".
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t* slash = wcsrchr(exe, L'\\');
  if (slash)
    *slash = 0;
  swprintf_s(g_profileDir, L"%s\\tlmp_profiles", exe);
  CreateDirectoryW(g_profileDir, nullptr);
  wcscat_s(g_profileDir, L"\\");
  wcscat_s(g_profileDir, name);
  CreateDirectoryW(g_profileDir, nullptr);
  seedSettings(); // reads the real %APPDATA%, so must run before it is overridden below

  // WinMain's folder setup (0x5CCA70, which creates Save/ and Mods/) builds paths from
  // the APPDATA environment variable rather than SHGetSpecialFolderPathW, so redirect
  // that too. Without it the save directory never exists in the profile and character
  // saves fail silently.
  SetEnvironmentVariableW(L"APPDATA", g_profileDir);
  for (const wchar_t* sub : {L"\\Runic Games\\Torchlight\\Save", L"\\Runic Games\\Torchlight\\Mods"})
  {
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s%s", g_profileDir, sub);
    CreateDirectoryW(dir, nullptr);
  }

  DWORD old;
  if (!VirtualProtect(kIatSHGetSpecialFolderPathW, sizeof(void*), PAGE_READWRITE, &old))
  {
    g_profileDir[0] = 0;
    return;
  }
  g_original = reinterpret_cast<SHGetSpecialFolderPathWFn>(*kIatSHGetSpecialFolderPathW);
  *kIatSHGetSpecialFolderPathW = reinterpret_cast<void*>(&hookedSHGetSpecialFolderPathW);
  VirtualProtect(kIatSHGetSpecialFolderPathW, sizeof(void*), old, &old);
}

const wchar_t* activeProfileDir()
{
  return g_profileDir[0] ? g_profileDir : nullptr;
}

} // namespace tlmp::profile
