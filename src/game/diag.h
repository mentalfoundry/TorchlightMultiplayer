#pragma once

#include <cstdint>

// Runtime reverse-engineering helpers. These read MSVC RTTI from live objects so
// addresses found statically can be confirmed in the running game.
namespace tlmp::diag {

// Reads a dword without faulting; returns false for unmapped memory.
bool readU32(uintptr_t addr, uint32_t& out);

// Class name (e.g. ".?AVCPlayer@@") of the object at p, if p points to a polymorphic
// object whose vtable lives in Torchlight.exe; nullptr otherwise.
const char* rttiName(uintptr_t p);

// Logs every .data global that points at a Torchlight object, grouped by class.
void dumpSingletons();

// Logs pointer-sized fields inside an object that point at other RTTI objects.
void dumpObjectLinks(uintptr_t obj, uint32_t size, const char* label);

// Logs every field that looks like a VS2008 std::wstring with printable contents.
void dumpObjectStrings(uintptr_t obj, uint32_t size, const char* label);

// Hash of every unit in a level's owned list (+0x64): class name and position rounded to
// 0.1. Two levels with equal fingerprints have the same props/monsters in the same places.
uint32_t levelFingerprint(uintptr_t level, uint32_t* unitCount);

// Reads a VS2008 std::wstring (28 bytes: alloc, 8-wchar SSO buffer/pointer, size, capacity).
// Printable ASCII only (good for identifiers); readWStringAny accepts any character >= 0x20.
bool readWString(uintptr_t addr, wchar_t* out, size_t outLen);
bool readWStringAny(uintptr_t addr, wchar_t* out, size_t outLen);
// VS2008 std::string (char): +4 buf[16] / ptr once capacity >= 16, +0x14 size, +0x18 capacity.
// Printable ASCII only.
bool readString(uintptr_t addr, char* out, size_t outLen);

} // namespace tlmp::diag
