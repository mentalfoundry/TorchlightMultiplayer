#include "game/anim.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/floor.h"
#include "game/internal.h"
#include "game/monsters.h"
#include "log.h"
#include "net/session.h"

#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tlmp::game::anim {

using net::Msg;

constexpr float kFollowUpWindow = 3.0f; // a queued animation counts as a follow-up this long after a one-shot
constexpr int kMaxArgs = 4;

static net::Session* g_session = nullptr;
static bool g_replaying = false;

enum Kind : uint8_t
{
  kPlayer = 0,
  kPet = 1,
  kMonster = 2,
};

static const char* const kCallNames[] = {"play", "crossfade", "queue"};

// Last one-shot sent per animation object, so only the follow-ups the game queues right after
// one are sent (not idle/run transitions from movement).
static std::unordered_map<const void*, float> g_lastOneShot;
static std::unordered_set<std::string> g_seenNames; // logged once each
static unsigned g_sent = 0, g_played = 0, g_missing = 0;
static std::string g_lastReplayed[net::kMaxPlayers]; // test mode: last animation replayed per player

static bool connected()
{
  return g_session && g_session->isConnected();
}

static void* animationOf(const void* unit)
{
  return unit ? engine::field<void*>(unit, engine::kUnitAnimation) : nullptr;
}

static bool nameAt(const void* animObject, uint32_t index, char* out, size_t outLen)
{
  uintptr_t set = 0, count = 0, begin = 0, end = 0;
  auto obj = reinterpret_cast<uintptr_t>(animObject);
  uint32_t v;
  if (!diag::readU32(obj + engine::kAnimationSet, v) || !v)
    return false;
  set = v;
  if (!diag::readU32(set + engine::kAnimSetCount, v))
    return false;
  count = v;
  if (!diag::readU32(set + engine::kAnimSetNames, v))
    return false;
  begin = v;
  if (!diag::readU32(set + engine::kAnimSetNames + 4, v))
    return false;
  end = v;
  uintptr_t stored = end > begin ? (end - begin) / engine::kAnimNameSize : 0;
  if (index >= count || index >= stored)
    return false;
  return diag::readString(begin + index * engine::kAnimNameSize, out, outLen);
}

static bool sameName(const char* a, const char* b)
{
  for (; *a && *b; a++, b++)
    if (std::toupper(static_cast<unsigned char>(*a)) != std::toupper(static_cast<unsigned char>(*b)))
      return false;
  return *a == *b;
}

// Index of `name` in the object's animation set: `hint` first (same class, same list), then a scan.
static int findIndex(const void* animObject, const char* name, uint32_t hint)
{
  char buf[64];
  if (nameAt(animObject, hint, buf, sizeof(buf)) && sameName(buf, name))
    return static_cast<int>(hint);
  for (uint32_t i = 0; i < 512; i++)
  {
    if (!nameAt(animObject, i, buf, sizeof(buf)))
    {
      if (i > 0)
        break;
      continue;
    }
    if (sameName(buf, name))
      return static_cast<int>(i);
  }
  return -1;
}

static void replay(engine::Unit* unit, Call call, const char* name, uint32_t hint, const uint32_t* args)
{
  void* obj = animationOf(unit);
  if (!obj)
    return;
  int index = findIndex(obj, name, hint);
  if (index < 0)
  {
    if (++g_missing <= 20)
      TLMP_LOG("anim: copy %p has no animation '%s'", unit, name);
    return;
  }
  g_replaying = true;
  auto idx = static_cast<uint32_t>(index);
  switch (call)
  {
    case Call::Play:
      reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t)>(engine::kAnimPlay)(
        obj, idx, args[0], args[1], args[2]);
      break;
    case Call::Crossfade:
      reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(
        engine::kAnimCrossfade)(obj, idx, args[0], args[1], args[2], args[3]);
      break;
    case Call::Queue:
      reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t)>(engine::kAnimQueue)(
        obj, idx, args[0], args[1], args[2]);
      break;
  }
  g_replaying = false;
  g_played++;
}

static void writeAnim(net::Writer& w, uint8_t slot, uint8_t kind, uint32_t monsterId, Call call, const char* name,
                      uint32_t index, const uint32_t* args, int argCount)
{
  w.u8(slot);
  w.u32(levelId(currentLevel()));
  w.u8(kind);
  w.u32(monsterId);
  w.u8(static_cast<uint8_t>(call));
  w.str(name);
  w.u32(index);
  w.u8(static_cast<uint8_t>(argCount));
  for (int i = 0; i < argCount; i++)
    w.u32(args[i]);
}

void attach(net::Session* session)
{
  g_session = session;
}

bool replaying()
{
  return g_replaying;
}

void onCall(void* animObject, Call call, uint32_t index, const uint32_t* args, int argCount)
{
  if (g_replaying || !connected() || !animObject)
    return;

  // One-shots (loop = 0) are attacks and casts; looping ones are idle/run/walk, which copies make
  // themselves. A queued animation is sent only as the follow-up of a one-shot just sent. (Checked
  // first: this runs for every animation of every unit.)
  bool loop = (args[0] & 0xFF) != 0;
  float now = clock();
  if (call == Call::Queue)
  {
    auto it = g_lastOneShot.find(animObject);
    if (it == g_lastOneShot.end() || now - it->second > kFollowUpWindow)
      return;
  }
  else if (loop)
    return;

  uint8_t kind;
  uint32_t monsterId = 0;
  uintptr_t me = localPlayer();
  if (me && animationOf(reinterpret_cast<void*>(me)) == animObject)
    kind = kPlayer;
  else if (uintptr_t pet = localPet(); pet && animationOf(reinterpret_cast<void*>(pet)) == animObject)
    kind = kPet;
  else if (floor::ownsCurrentLevel() && (monsterId = monsters::trackedIdByAnimation(animObject)) != 0)
    kind = kMonster;
  else
    return;

  if (call != Call::Queue)
  {
    if (g_lastOneShot.size() > 4096)
      g_lastOneShot.clear(); // stale animation objects of units long gone
    g_lastOneShot[animObject] = now;
  }

  char name[64];
  if (!nameAt(animObject, index, name, sizeof(name)))
    return;
  // Deaths are the game's own on every machine (Kill plays DIE / DEATH_* on the copy).
  if (_strnicmp(name, "DIE", 3) == 0 || _strnicmp(name, "DEATH", 5) == 0)
    return;
  if (kind != kMonster && g_seenNames.insert(name).second)
    TLMP_LOG("anim: my %s plays '%s' (%s, index %u)", kind == kPet ? "pet" : "character", name,
             kCallNames[static_cast<int>(call)], index);

  net::Writer w(Msg::AnimPlay);
  writeAnim(w, g_session->localSlot(), kind, monsterId, call, name, index, args, argCount);
  if (g_session->isHost())
    g_session->sendToAll(w, true);
  else
    g_session->sendTo(0, w, true);
  g_sent++;
  if (g_sent % 200 == 0)
    TLMP_LOG("anim: %u animations sent, %u played on copies, %u missing", g_sent, g_played, g_missing);
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (type != Msg::AnimPlay || !connected())
    return;
  uint8_t slot = r.u8();
  uint32_t lid = r.u32();
  uint8_t kind = r.u8();
  uint32_t monsterId = r.u32();
  auto call = static_cast<Call>(r.u8());
  std::string name = r.str();
  uint32_t index = r.u32();
  int argCount = r.u8();
  uint32_t args[kMaxArgs] = {};
  for (int i = 0; i < argCount && i < kMaxArgs; i++)
    args[i] = r.u32();
  if (!r.ok() || argCount > kMaxArgs || static_cast<uint8_t>(call) > 2)
    return;

  if (g_session->isHost())
  {
    // From a client: the slot is whoever sent it. Show it here, and pass it on to the others.
    slot = fromSlot;
    net::Writer w(Msg::AnimPlay);
    w.u8(slot);
    w.u32(lid);
    w.u8(kind);
    w.u32(monsterId);
    w.u8(static_cast<uint8_t>(call));
    w.str(name);
    w.u32(index);
    w.u8(static_cast<uint8_t>(argCount));
    for (int i = 0; i < argCount; i++)
      w.u32(args[i]);
    for (uint8_t s = 1; s < net::kMaxPlayers; s++)
      if (s != fromSlot && g_session->player(s).active)
        g_session->sendTo(s, w, true);
  }

  if (lid != levelId(currentLevel()))
    return;
  engine::Unit* unit = kind == kPlayer ? avatarOf(slot)
                       : kind == kPet  ? avatarPetOf(slot)
                                       : monsters::puppetById(monsterId); // null where it's the real one
  if (unit)
  {
    replay(unit, call, name.c_str(), index, args);
    if (kind != kMonster && slot < net::kMaxPlayers)
      g_lastReplayed[slot] = name;
  }
}

bool playOneShot(void* unit, const char* name)
{
  void* obj = animationOf(unit);
  int index = obj ? findIndex(obj, name, 0) : -1;
  if (index < 0)
    return false;
  // The way 0x486070 starts an attack: crossfade, not looping, blend 0.5, rate 1, own length.
  uint32_t blend, rate, duration;
  float b = 0.5f, rt = 1.0f;
  std::memcpy(&blend, &b, 4);
  std::memcpy(&rate, &rt, 4);
  duration = *reinterpret_cast<const uint32_t*>(0x00A91FB0);
  reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(engine::kAnimCrossfade)(
    obj, static_cast<uint32_t>(index), 0, blend, rate, duration);
  return true;
}

std::string lastReplayed(uint8_t slot)
{
  return slot < net::kMaxPlayers ? g_lastReplayed[slot] : std::string();
}

} // namespace tlmp::game::anim
