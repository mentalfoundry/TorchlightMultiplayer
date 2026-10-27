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
  // each other.
  char name[64];
  std::snprintf(name, sizeof(name), "TLMP_%lu.log", GetCurrentProcessId());
  g_file = std::fopen(name, "w");
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
