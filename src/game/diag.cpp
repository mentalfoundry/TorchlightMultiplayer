#include "game/diag.h"
#include "crashlog.h"
#include "log.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace tlmp::diag {

// Torchlight.exe section bounds (GOG 1.7.8.70).
constexpr uintptr_t kRdataBegin = 0x00A85000;
constexpr uintptr_t kRdataEnd = 0x00C11200;
constexpr uintptr_t kDataBegin = 0x00C12000;
constexpr uintptr_t kDataEnd = 0x00F64C44;

static bool readU32Raw(uintptr_t addr, uint32_t& out)
{
  __try
  {
    out = *reinterpret_cast<const volatile uint32_t*>(addr);
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

bool readU32(uintptr_t addr, uint32_t& out)
{
  crashlog::g_probing = 1;
  bool ok = readU32Raw(addr, out);
  crashlog::g_probing = 0;
  return ok;
}

const char* rttiName(uintptr_t p)
{
  // MSVC x86: obj -> vtable; vtable[-1] -> CompleteObjectLocator
  //   COL { sig=0, offset, cdOffset, TypeDescriptor*, ClassHierarchyDescriptor* }
  //   TypeDescriptor { vftable, spare, char name[] }
  if (p < 0x10000 || (p & 3))
    return nullptr;
  uint32_t vt, col, sig, td;
  if (!readU32(p, vt) || vt < kRdataBegin || vt >= kRdataEnd)
    return nullptr;
  if (!readU32(vt - 4, col) || col < kRdataBegin || col >= kRdataEnd)
    return nullptr;
  if (!readU32(col, sig) || sig != 0 || !readU32(col + 12, td))
    return nullptr;
  if (td < kRdataBegin || td >= kDataEnd)
    return nullptr;
  const char* name = reinterpret_cast<const char*>(td + 8);
  uint32_t head;
  if (!readU32(td + 8, head) || std::memcmp(&head, ".?A", 3) != 0)
    return nullptr;
  return name;
}

void dumpSingletons()
{
  std::map<std::string, std::vector<uintptr_t>> byClass;
  for (uintptr_t g = kDataBegin; g < kDataEnd; g += 4)
  {
    uint32_t v;
    if (!readU32(g, v))
      continue;
    // Skip pointers into the image itself (vtables, static tables).
    if (v >= 0x00400000 && v < kDataEnd)
      continue;
    if (const char* name = rttiName(v))
      byClass[name].push_back(g);
  }
  TLMP_LOG("diag: %u classes referenced from .data globals", static_cast<unsigned>(byClass.size()));
  for (const auto& [name, globals] : byClass)
  {
    char line[512];
    int n = std::snprintf(line, sizeof(line), "diag:   %-48s", name.c_str());
    for (size_t i = 0; i < globals.size() && i < 6 && n < 440; i++)
    {
      uint32_t v = 0;
      readU32(globals[i], v);
      n += std::snprintf(line + n, sizeof(line) - n, " [%08x]=%08x", static_cast<unsigned>(globals[i]), v);
    }
    if (globals.size() > 6)
      std::snprintf(line + n, sizeof(line) - n, " (+%u more)", static_cast<unsigned>(globals.size() - 6));
    TLMP_LOG("%s", line);
  }
}

static bool readWStringImpl(uintptr_t addr, wchar_t* out, size_t outLen, bool asciiOnly)
{
  // VS2008 _String_val<wchar_t>: +0 allocator (4), +4 union { wchar_t buf[8]; wchar_t* ptr; },
  // +0x14 size, +0x18 capacity. The pointer is used once capacity >= 8.
  uint32_t size, cap;
  if (!readU32(addr + 0x14, size) || !readU32(addr + 0x18, cap))
    return false;
  if (size >= outLen || size > cap || cap > 0x10000 || cap < 7)
    return false;
  uintptr_t chars = addr + 4;
  if (cap >= 8)
  {
    uint32_t p;
    if (!readU32(addr + 4, p))
      return false;
    chars = p;
  }
  for (uint32_t i = 0; i < size; i++)
  {
    uint32_t pair;
    if (!readU32((chars + i * 2) & ~3u, pair))
      return false;
    wchar_t c = static_cast<wchar_t>(((chars + i * 2) & 2) ? (pair >> 16) : (pair & 0xFFFF));
    if (c < 0x20 || (asciiOnly && c > 0x7E))
      return false;
    out[i] = c;
  }
  out[size] = 0;
  return true;
}

bool readString(uintptr_t addr, char* out, size_t outLen)
{
  uint32_t size, cap;
  if (!readU32(addr + 0x14, size) || !readU32(addr + 0x18, cap))
    return false;
  if (size >= outLen || size > cap || cap > 0x10000 || cap < 15)
    return false;
  uintptr_t chars = addr + 4;
  if (cap >= 16)
  {
    uint32_t p;
    if (!readU32(addr + 4, p))
      return false;
    chars = p;
  }
  for (uint32_t i = 0; i < size; i++)
  {
    uint32_t word;
    if (!readU32((chars + i) & ~3u, word))
      return false;
    char c = static_cast<char>(word >> (((chars + i) & 3) * 8));
    if (c < 0x20 || c > 0x7E)
      return false;
    out[i] = c;
  }
  out[size] = 0;
  return true;
}

bool readWString(uintptr_t addr, wchar_t* out, size_t outLen)
{
  return readWStringImpl(addr, out, outLen, true);
}

bool readWStringAny(uintptr_t addr, wchar_t* out, size_t outLen)
{
  return readWStringImpl(addr, out, outLen, false);
}

void dumpObjectStrings(uintptr_t obj, uint32_t size, const char* label)
{
  TLMP_LOG("diag: strings of %s @%08x", label, static_cast<unsigned>(obj));
  for (uint32_t off = 0; off + 28 <= size; off += 4)
  {
    wchar_t buf[128];
    if (readWString(obj + off, buf, 128) && buf[0])
      TLMP_LOG("diag:   +%03x \"%ls\"", off, buf);
  }
}

uint32_t levelFingerprint(uintptr_t level, uint32_t* unitCount)
{
  // CLevel keeps unit lists at +0x5C, +0x64 and +0x80 (see RemoveUnit 0x4F7160); each is a
  // pointer to a list object whose [0] is the head node; node { unit*, next* }.
  uint32_t h = 2166136261u, n = 0;
  for (uint32_t listOff : {0x5Cu, 0x64u, 0x80u})
  {
    uint32_t list, node;
    if (!readU32(level + listOff, list) || !list || !readU32(list, node))
      continue;
    h = (h ^ listOff) * 16777619u;
    for (uint32_t guard = 0; node && guard < 20000; guard++)
    {
      uint32_t unit, next;
      if (!readU32(node, unit) || !readU32(node + 4, next))
        break;
      const char* name = rttiName(unit);
      if (name && std::strcmp(name, ".?AVCPlayer@@") != 0)
      {
        for (const char* c = name; *c; c++)
          h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
        for (uint32_t i = 0; i < 3; i++)
        {
          uint32_t bits = 0;
          readU32(unit + 0x70 + i * 4, bits);
          float f;
          std::memcpy(&f, &bits, 4);
          h = (h ^ static_cast<uint32_t>(static_cast<int32_t>(f * 10.0f))) * 16777619u;
        }
        n++;
      }
      node = next;
    }
  }
  if (unitCount)
    *unitCount = n;
  return h;
}

void dumpObjectLinks(uintptr_t obj, uint32_t size, const char* label)
{
  TLMP_LOG("diag: links of %s @%08x (%s)", label, static_cast<unsigned>(obj), rttiName(obj) ? rttiName(obj) : "?");
  for (uint32_t off = 0; off < size; off += 4)
  {
    uint32_t v;
    if (!readU32(obj + off, v))
      break;
    if (const char* name = rttiName(v))
      TLMP_LOG("diag:   +%03x -> %08x %s", off, v, name);
  }
}

} // namespace tlmp::diag
