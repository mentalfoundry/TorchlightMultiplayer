#include "game/levelguard.h"
#include "game/diag.h"
#include "log.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace tlmp::game::levelguard {

namespace {

// The game's read-only data, where every vtable lives.
constexpr uint32_t kVtablesLo = 0x00A80000;
constexpr uint32_t kVtablesHi = 0x00AC0000;
// CLevel lists the activity pass walks: units +0x64, items +0x68. Each points to a list object
// whose first word is the head node; a node is { object*, next*, prev* }. 0x4F40A0 unlinks a node
// (thiscall(list, node), ret 4), as RemoveUnit does before freeing it.
constexpr uint32_t kLevelUnits = 0x64;
constexpr uint32_t kLevelItems = 0x68;
constexpr uint32_t kLevelActive = 0x5C; // the per-frame update list (0x4F8440)
using UnlinkFn = void(__thiscall*)(void* list, void* node);
const auto Unlink = reinterpret_cast<UnlinkFn>(0x004F40A0);

struct Freed
{
  void* unit = nullptr;
  void* callers[4] = {};
  uint32_t tick = 0;
};
Freed g_freed[64];
unsigned g_next = 0;

const char* describe(void* unit);

bool live(uint32_t object)
{
  uint32_t vtable = 0;
  return object && diag::readU32(object, vtable) && vtable >= kVtablesLo && vtable < kVtablesHi;
}

void check(void* level, uint32_t offset, const char* what)
{
  uint32_t list = 0, node = 0;
  if (!diag::readU32(reinterpret_cast<uintptr_t>(level) + offset, list) || !list || !diag::readU32(list, node))
    return;
  for (int guard = 0; node && guard < 100000; guard++)
  {
    uint32_t object = 0, next = 0;
    if (!diag::readU32(node, object) || !diag::readU32(node + 4, next))
    {
      TLMP_LOG("levelguard: %s list of level %p has an unreadable node %08x; stopped checking", what, level, node);
      return;
    }
    // A unit's resources (+0x60) belong to its level ([res+0xC]): one made for another level (since
    // freed) has a dangling tag table, and the activity check's HasTag (0x480760) crashes on it.
    uint32_t res = 0, resLevel = 0;
    bool foreign = offset != kLevelItems && live(object) && diag::readU32(object + 0x60, res) && res &&
                   diag::readU32(res + 0xC, resLevel) && resLevel != reinterpret_cast<uintptr_t>(level);
    if (foreign)
    {
      uint32_t guidLo = 0, guidHi = 0;
      diag::readU32(object + 0x168, guidLo);
      diag::readU32(object + 0x16C, guidHi);
      const char* cls = diag::rttiName(object);
      TLMP_LOG("levelguard: unit %08x (%s, GUID %08x%08x) in level %p's list belongs to level %08x (resources %08x)%s; "
               "unlinked",
               object, cls ? cls : "?", guidHi, guidLo, level, resLevel, res, describe(reinterpret_cast<void*>(object)));
      Unlink(reinterpret_cast<void*>(list), reinterpret_cast<void*>(node));
      node = next;
      continue;
    }
    if (!live(object))
    {
      uint32_t first = 0;
      diag::readU32(object, first);
      const Freed* f = nullptr;
      for (const Freed& e : g_freed)
        if (e.unit == reinterpret_cast<void*>(object))
          f = &e;
      if (f)
        TLMP_LOG("levelguard: %s list of level %p still held %08x (first word %08x), freed %lu ms ago from %p <- %p <- "
                 "%p <- %p; unlinked",
                 what, level, object, first, static_cast<unsigned long>(GetTickCount() - f->tick), f->callers[0],
                 f->callers[1], f->callers[2], f->callers[3]);
      else
        TLMP_LOG("levelguard: %s list of level %p held a dead entry %08x (first word %08x), not freed as a character; "
                 "unlinked",
                 what, level, object, first);
      Unlink(reinterpret_cast<void*>(list), reinterpret_cast<void*>(node)); // the node itself is left as is (leaked)
    }
    node = next;
  }
}

} // namespace

DescribeFn g_describe = nullptr;

bool unitResourcesOk(void* unit, void* level)
{
  uint32_t res = 0, resLevel = 0;
  const uintptr_t u = reinterpret_cast<uintptr_t>(unit);
  if (!unit || !level || !diag::readU32(u + 0x60, res) || !res)
    return true;
  const bool readable = diag::readU32(res + 0xC, resLevel);
  if (readable && resLevel == reinterpret_cast<uintptr_t>(level))
    return true;
  static void* lastLogged = nullptr;
  if (unit != lastLogged)
  {
    lastLogged = unit;
    uint32_t guidLo = 0, guidHi = 0, vtable = 0, type = 0;
    diag::readU32(u, vtable);
    diag::readU32(u + 0x168, guidLo);
    diag::readU32(u + 0x16C, guidHi);
    diag::readU32(u + 0x174, type);
    const char* cls = diag::rttiName(u);
    const Freed* f = nullptr;
    for (const Freed& e : g_freed)
      if (e.unit == unit)
        f = &e;
    if (readable)
      TLMP_LOG("levelguard: unit %p (%s, GUID %08x%08x) is updated on level %p but its resources belong to level %08x%s",
               unit, cls ? cls : "?", guidHi, guidLo, level, resLevel, describe(unit));
    else
      TLMP_LOG("levelguard: unit %p (vtable %08x, %s, GUID %08x%08x, type %08x) is updated on level %p but its resources "
               "pointer %08x is garbage%s%s",
               unit, vtable, cls ? cls : "?", guidHi, guidLo, type, level, res, describe(unit),
               f ? " -- it was freed as a character" : "");
    if (f)
      TLMP_LOG("levelguard:   freed %lu ms ago from %p <- %p <- %p <- %p", static_cast<unsigned long>(GetTickCount() - f->tick),
               f->callers[0], f->callers[1], f->callers[2], f->callers[3]);
    for (uint32_t off = 0; off < 0x200; off += 0x20)
    {
      uint32_t w[8] = {};
      for (int i = 0; i < 8; i++)
        diag::readU32(u + off + i * 4, w[i]);
      TLMP_LOG("levelguard:   +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", off, w[0], w[1], w[2], w[3], w[4], w[5],
               w[6], w[7]);
    }
  }
  return false;
}

void setDescriber(DescribeFn fn)
{
  g_describe = fn;
}

namespace {
// A minion's master (+0x5B4, set by 0x499E70). The game clears it only when the master dies (the
// minion's update, 0x4AF235), never when the master is freed: a minion left on the floor after its
// master is gone -- a stand-in or copy removed while its summons stay -- then points at freed
// memory, and the activity check's HasTag on the master (0x48C0A3) crashes.
constexpr uint32_t kMaster = 0x5B4;

void forgetMaster(uintptr_t level, uintptr_t dying, void* const* from)
{
  for (uint32_t offset : {kLevelUnits, kLevelActive})
  {
    uint32_t list = 0, node = 0;
    if (!level || !diag::readU32(level + offset, list) || !list || !diag::readU32(list, node))
      continue;
    for (int guard = 0; node && guard < 100000; guard++)
    {
      uint32_t object = 0, next = 0, master = 0;
      if (!diag::readU32(node, object) || !diag::readU32(node + 4, next))
        break;
      if (live(object) && diag::readU32(object + kMaster, master) && master == dying)
      {
        std::string minion = describe(reinterpret_cast<void*>(object)); // one buffer: copy before the next
        TLMP_LOG("levelguard: unit %08x (%s)%s outlives its master %08x%s (freed from %p <- %p <- %p); master cleared", object,
                 diag::rttiName(object) ? diag::rttiName(object) : "?", minion.c_str(), static_cast<uint32_t>(dying),
                 describe(reinterpret_cast<void*>(dying)), from[0], from[1], from[2]);
        *reinterpret_cast<uint32_t*>(object + kMaster) = 0;
      }
      node = next;
    }
  }
}
} // namespace

void noteDestroyed(void* unit, void* const* callers, int count, void* currentLevel)
{
  const uintptr_t u = reinterpret_cast<uintptr_t>(unit);
  uint32_t res = 0, level = 0;
  if (diag::readU32(u + 0x60, res) && res)
    diag::readU32(res + 0xC, level);
  void* from[3] = {};
  for (int i = 0; i < 3 && i < count; i++)
    from[i] = callers[i];
  forgetMaster(level, u, from);
  if (currentLevel && reinterpret_cast<uintptr_t>(currentLevel) != level)
    forgetMaster(reinterpret_cast<uintptr_t>(currentLevel), u, from);

  Freed& f = g_freed[g_next++ % 64];
  f.unit = unit;
  for (int i = 0; i < 4; i++)
    f.callers[i] = i < count ? callers[i] : nullptr;
  f.tick = GetTickCount();
}

void checkLists(void* level)
{
  if (!level)
    return;
  check(level, kLevelActive, "active");
  check(level, kLevelUnits, "unit");
  check(level, kLevelItems, "item");
}

namespace {
const char* describe(void* unit)
{
  return g_describe ? g_describe(unit) : "";
}
} // namespace

} // namespace tlmp::game::levelguard
