#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace tlmp::log {

static std::mutex g_mutex;
static FILE* g_file = nullptr;

void open()
{
  std::lock_guard lock(g_mutex);
  if (g_file)
    return;

  // One log per process so several local instances (used for testing) don't clobber
  // each other, in TLMP_logs next to Torchlight.exe.
  wchar_t dir[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
  while (n > 0 && dir[n - 1] != L'\\')
    n--;
  dir[n] = 0;
  wchar_t name[MAX_PATH + 64];
  std::swprintf(name, MAX_PATH + 64, L"%lsTLMP_logs", dir);
  CreateDirectoryW(name, nullptr); // fine if it exists
  std::swprintf(name, MAX_PATH + 64, L"%lsTLMP_logs\\TLMP_%lu.log", dir, GetCurrentProcessId());
  g_file = _wfopen(name, L"w");
}

void close()
{
  std::lock_guard lock(g_mutex);
  if (g_file)
  {
    std::fclose(g_file);
    g_file = nullptr;
  }
}

void write(const char* fmt, ...)
{
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  SYSTEMTIME t;
  GetLocalTime(&t);

  std::lock_guard lock(g_mutex);
  if (g_file)
  {
    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
    std::fflush(g_file);
  }
  OutputDebugStringA(buf);
  OutputDebugStringA("\n");
}

} // namespace tlmp::log
