#pragma once

// Passing std::wstring by value to game functions. The game was built with VS2008, whose
// std::wstring is a 28-byte object (allocator, 8-wchar buffer/pointer, size, capacity).
// We build one with MSVCP90's own constructor so its heap buffer comes from the CRT the
// game frees it with, then pass the 28 bytes by value. MSVC callees destroy by-value
// arguments, so after the call the bytes must not be destroyed again on our side.

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace tlmp::vs {

struct WString
{
  uint8_t raw[28];
};
static_assert(sizeof(WString) == 28);

inline HMODULE msvcp90()
{
  static HMODULE mod = GetModuleHandleW(L"msvcp90.dll");
  return mod;
}

// Constructs a VS2008 std::wstring holding text. Ownership passes to whoever receives it
// by value.
inline WString make(const wchar_t* text)
{
  using CtorFn = void*(__thiscall*)(void* self, const wchar_t* text);
  static auto ctor = reinterpret_cast<CtorFn>(GetProcAddress(
    msvcp90(), "??0?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAE@PB_W@Z"));
  WString s;
  std::memset(&s, 0, sizeof(s));
  ctor(&s, text);
  return s;
}

// Destroys a VS2008 std::wstring in place (frees its buffer through MSVCP90).
inline void destroy(void* str)
{
  using DtorFn = void(__thiscall*)(void* self);
  static auto dtor = reinterpret_cast<DtorFn>(
    GetProcAddress(msvcp90(), "??1?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAE@XZ"));
  dtor(str);
}

} // namespace tlmp::vs
