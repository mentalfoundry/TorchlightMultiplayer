#include "game/savestate.h"
#include "game/engine.h"
#include "log.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace tlmp::game::savestate {

namespace {

struct Crt
{
  using WfopenFn = FILE*(__cdecl*)(const wchar_t*, const wchar_t*);
  using FcloseFn = int(__cdecl*)(FILE*);
  using FreadFn = size_t(__cdecl*)(void*, size_t, size_t, FILE*);
  using FwriteFn = size_t(__cdecl*)(const void*, size_t, size_t, FILE*);
  using FseekFn = int(__cdecl*)(FILE*, long, int);
  using FtellFn = long(__cdecl*)(FILE*);
  WfopenFn wfopen = nullptr;
  FcloseFn fclose = nullptr;
  FreadFn fread = nullptr;
  FwriteFn fwrite = nullptr;
  FseekFn fseek = nullptr;
  FtellFn ftell = nullptr;
  bool ok = false;
};

const Crt& crt()
{
  static Crt c = [] {
    Crt r;
    HMODULE m = GetModuleHandleW(L"msvcr90.dll");
    if (!m)
      return r;
    r.wfopen = reinterpret_cast<Crt::WfopenFn>(GetProcAddress(m, "_wfopen"));
    r.fclose = reinterpret_cast<Crt::FcloseFn>(GetProcAddress(m, "fclose"));
    r.fread = reinterpret_cast<Crt::FreadFn>(GetProcAddress(m, "fread"));
    r.fwrite = reinterpret_cast<Crt::FwriteFn>(GetProcAddress(m, "fwrite"));
    r.fseek = reinterpret_cast<Crt::FseekFn>(GetProcAddress(m, "fseek"));
    r.ftell = reinterpret_cast<Crt::FtellFn>(GetProcAddress(m, "ftell"));
    r.ok = r.wfopen && r.fclose && r.fread && r.fwrite && r.fseek && r.ftell;
    if (!r.ok)
      TLMP_LOG("savestate: msvcr90 file functions not found; units can't be sent with their saved state");
    return r;
  }();
  return c;
}

std::wstring tempPath()
{
  wchar_t dir[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, dir);
  std::wstring p = n ? std::wstring(dir, n) : std::wstring(L".\\");
  return p + L"tlmp_state_" + std::to_wstring(GetCurrentProcessId()) + L".bin";
}

} // namespace

bool available()
{
  return crt().ok;
}

void* create(uint32_t size, void*(__thiscall* ctor)(void*))
{
  void* mem = available() ? engine::ogreNew(size) : nullptr;
  return mem ? ctor(mem) : nullptr;
}

void destroy(void* state)
{
  if (state)
    engine::virtualFn<void(__thiscall*)(void*, unsigned)>(state, 0)(state, 1);
}

bool toBytes(void* state, WriteFn write, std::vector<uint8_t>& out)
{
  const Crt& c = crt();
  if (!c.ok || !state)
    return false;
  std::wstring path = tempPath();
  bool ok = false;
  if (FILE* f = c.wfopen(path.c_str(), L"w+b"))
  {
    write(state, f);
    long size = c.ftell(f);
    if (size > 0 && size < (1 << 20))
    {
      out.resize(static_cast<size_t>(size));
      c.fseek(f, 0, SEEK_SET);
      ok = c.fread(out.data(), 1, out.size(), f) == out.size();
    }
    c.fclose(f);
  }
  DeleteFileW(path.c_str());
  return ok;
}

bool fromBytes(void* state, ReadFn read, uint32_t version, const std::vector<uint8_t>& bytes)
{
  const Crt& c = crt();
  if (!c.ok || !state || bytes.empty())
    return false;
  std::wstring path = tempPath();
  FILE* f = c.wfopen(path.c_str(), L"w+b");
  if (!f)
    return false;
  bool ok = c.fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  c.fseek(f, 0, SEEK_SET);
  if (ok)
    read(state, f, version);
  c.fclose(f);
  DeleteFileW(path.c_str());
  return ok;
}

} // namespace tlmp::game::savestate
