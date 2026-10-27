#include "game/minions.h"
#include "game/floor.h"
#include "game/internal.h"
#include "log.h"
#include "net/session.h"

#include <vector>

namespace tlmp::game::minions {

using net::Msg;

static net::Session* g_session = nullptr;

constexpr float kReportInterval = 0.2f;
constexpr float kAdoptWait = 1.5f;   // a reported minion gets a new copy only if no replayed one came by then
constexpr float kStrayGrace = 3.0f;  // a stand-in's minion nobody reports is removed after this
constexpr uint8_t kMaxMinions = 32;
constexpr uint32_t kMinionOwner = 0x5B4;

// --- reporting our minions ---------------------------------------------------------------------

static std::vector<engine::Unit*> g_reported; // our minions, in the order of our last report

static bool isDead(engine::Unit* u)
{
  int s = engine::field<int>(u, engine::kUnitAIState);
  return s == engine::kAIStateDying || s == engine::kAIStateDead;
}

static void report()
{
  auto* me = reinterpret_cast<engine::Unit*>(localPlayer());
  engine::Level* level = currentLevel();
  if (!me || !level)
    return;
  g_reported.clear();
  auto** it = engine::field<engine::Unit**>(me, engine::kPlayerMinions);
  auto** end = engine::field<engine::Unit**>(me, engine::kPlayerMinions + 4);
  for (; it && it != end && g_reported.size() < kMaxMinions; ++it)
    if (*it && reinterpret_cast<uintptr_t>(*it) != localPet() && !isAvatarUnit(*it))
      g_reported.push_back(*it);
  static bool hadAny = false;
  if (g_reported.empty() && !hadAny)
    return; // nothing to say (an empty report is sent once, after the last minion goes)
  hadAny = !g_reported.empty();
  net::Writer w(Msg::Minions);
  w.u32(levelId(level));
  w.u8(static_cast<uint8_t>(g_reported.size()));
  for (engine::Unit* u : g_reported)
  {
    uint64_t guid = engine::field<uint64_t>(u, engine::kUnitGuid);
    Vec3 p = unitPosition(u);
    w.u32(static_cast<uint32_t>(guid));
    w.u32(static_cast<uint32_t>(guid >> 32));
    w.f32(p.x);
    w.f32(p.y);
    w.f32(p.z);
    w.f32(engine::field<float>(u, engine::kUnitHealth));
    w.u32(static_cast<uint32_t>(engine::GetMaxHealth(u)));
    w.u8(isDead(u) ? 1 : 0);
  }
  floor::sendToFloor(w, false);
}

engine::Unit* localMinion(uint8_t index)
{
  if (index >= g_reported.size())
    return nullptr;
  engine::Unit* u = g_reported[index];
  // Still ours? (it may have died and been freed since the report)
  auto* me = reinterpret_cast<engine::Unit*>(localPlayer());
  if (!me)
    return nullptr;
  auto** it = engine::field<engine::Unit**>(me, engine::kPlayerMinions);
  auto** end = engine::field<engine::Unit**>(me, engine::kPlayerMinions + 4);
  for (; it && it != end; ++it)
    if (*it == u)
      return isDead(u) ? nullptr : u;
  return nullptr;
}

// --- showing others' minions -------------------------------------------------------------------

struct Reported
{
  uint64_t guid = 0;
  Vec3 pos{};
  float health = 0.0f;
  uint32_t maxHealth = 0;
  bool dead = false;
};

struct Copy
{
  Puppet puppet;
  uint64_t guid = 0;
  bool spawnedByUs = false; // else adopted from the stand-in's own (replayed) summon
};

struct Remote
{
  uint32_t levelId = 0;
  std::vector<Reported> reported;
  std::vector<float> firstSeen; // per reported index, when it was first reported without a copy
  std::vector<Copy> copies;     // per reported index (unit null = none yet)
  std::vector<std::pair<engine::Unit*, float>> strays; // stand-in minions nobody reports, since when
};
static Remote g_remote[net::kMaxPlayers];

static void forget(Remote& r)
{
  r.reported.clear();
  r.firstSeen.clear();
  r.copies.clear();
  r.strays.clear();
}

static void removeCopy(Copy& c, const char* why)
{
  if (!c.puppet.unit)
    return;
  if (!isDead(c.puppet.unit))
    engine::kill(c.puppet.unit); // it dies as the minion did (or vanishes into its corpse)
  TLMP_LOG("minions: copy %p removed (%s)", c.puppet.unit, why);
  c = {};
}

bool copyOf(const void* unit, uint8_t* slot, uint8_t* index)
{
  if (!unit)
    return false;
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
    for (size_t i = 0; i < g_remote[s].copies.size(); i++)
      if (g_remote[s].copies[i].puppet.unit == unit)
      {
        *slot = s;
        *index = static_cast<uint8_t>(i);
        return true;
      }
  // A minion the stand-in summoned that isn't matched (yet) is still that player's.
  const void* owner = engine::field<void*>(unit, kMinionOwner);
  bool isPet = false;
  int s = owner ? avatarSlotOf(owner, &isPet) : -1;
  if (s >= 0 && !isPet)
  {
    *slot = static_cast<uint8_t>(s);
    *index = 0xFF;
    return true;
  }
  return false;
}

engine::Unit* anyCopy(uint8_t slot)
{
  if (slot < net::kMaxPlayers)
    for (const Copy& c : g_remote[slot].copies)
      if (c.puppet.unit && !isDead(c.puppet.unit))
        return c.puppet.unit;
  return nullptr;
}

unsigned copiesShown(uint8_t slot)
{
  unsigned n = 0;
  if (slot < net::kMaxPlayers)
    for (const Copy& c : g_remote[slot].copies)
      n += c.puppet.unit && !isDead(c.puppet.unit) ? 1 : 0;
  return n;
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (type != Msg::Minions || fromSlot >= net::kMaxPlayers)
    return;
  uint32_t lid = r.u32();
  uint8_t n = r.u8();
  std::vector<Reported> list;
  for (uint8_t i = 0; i < n && i < kMaxMinions; i++)
  {
    Reported m;
    uint32_t lo = r.u32(), hi = r.u32();
    m.guid = (static_cast<uint64_t>(hi) << 32) | lo;
    m.pos = {r.f32(), r.f32(), r.f32()};
    m.health = r.f32();
    m.maxHealth = r.u32();
    m.dead = r.u8() != 0;
    list.push_back(m);
  }
  if (!r.ok())
    return;
  Remote& rem = g_remote[fromSlot];
  if (lid != rem.levelId)
  {
    for (Copy& c : rem.copies)
      c = {}; // copies of another floor went with it
    forget(rem);
    rem.levelId = lid;
  }
  rem.reported = std::move(list);
  rem.firstSeen.resize(rem.reported.size(), -1.0f);
  // Minions gone from the report die here too.
  for (size_t i = rem.reported.size(); i < rem.copies.size(); i++)
    removeCopy(rem.copies[i], "no longer reported");
  rem.copies.resize(rem.reported.size());
}

// Minions of slot's stand-in that no copy stands for yet.
static void collectUnclaimed(uint8_t slot, Remote& rem, std::vector<engine::Unit*>& out)
{
  engine::Unit* standIn = avatarOf(slot);
  if (!standIn)
    return;
  auto** it = engine::field<engine::Unit**>(standIn, engine::kPlayerMinions);
  auto** end = engine::field<engine::Unit**>(standIn, engine::kPlayerMinions + 4);
  for (; it && it != end; ++it)
  {
    engine::Unit* u = *it;
    if (!u || isDead(u) || reinterpret_cast<engine::Unit*>(avatarPetOf(slot)) == u)
      continue;
    bool claimed = false;
    for (const Copy& c : rem.copies)
      claimed = claimed || c.puppet.unit == u;
    if (!claimed)
      out.push_back(u);
  }
}

static void show(uint8_t slot, Remote& rem, engine::Level* level, float now)
{
  std::vector<engine::Unit*> unclaimed;
  collectUnclaimed(slot, rem, unclaimed);
  for (size_t i = 0; i < rem.reported.size(); i++)
  {
    const Reported& m = rem.reported[i];
    Copy& c = rem.copies[i];
    if (c.puppet.unit && (c.guid != m.guid || isDead(c.puppet.unit)))
    {
      if (isDead(c.puppet.unit))
        c = {}; // it died here on its own: a new one if the minion still lives
      else
        removeCopy(c, "a different minion now");
    }
    if (m.dead)
    {
      removeCopy(c, "the minion died");
      continue;
    }
    if (!c.puppet.unit)
    {
      // Adopt what the stand-in's replayed summon made, or after a while make one ourselves.
      for (auto it = unclaimed.begin(); it != unclaimed.end(); ++it)
        if (engine::field<uint64_t>(*it, engine::kUnitGuid) == m.guid)
        {
          c.puppet = {*it, level, {}, -1.0f};
          c.guid = m.guid;
          c.spawnedByUs = false;
          unclaimed.erase(it);
          TLMP_LOG("minions: slot %u's minion %zu (%016llx) is shown by the stand-in's own summon %p", slot, i, m.guid,
                   c.puppet.unit);
          break;
        }
    }
    if (!c.puppet.unit)
    {
      if (rem.firstSeen[i] < 0.0f)
        rem.firstSeen[i] = now;
      if (now - rem.firstSeen[i] < kAdoptWait)
        continue;
      void* factory = engine::field<void*>(level, engine::kLevelUnitFactory);
      engine::Unit* unit = factory ? engine::createMonsterByGuid(factory, m.guid, 1) : nullptr;
      if (!unit)
      {
        rem.firstSeen[i] = now + 30.0f; // unknown type: don't retry every tick
        continue;
      }
      engine::SetAlignment(unit, engine::kAlignmentPlayer);
      engine::AddUnit(level, unit, &m.pos, false);
      c.puppet = {unit, level, m.pos, -1.0f};
      c.guid = m.guid;
      c.spawnedByUs = true;
      TLMP_LOG("minions: made a copy %p of slot %u's minion %zu (%016llx)", unit, slot, i, m.guid);
    }
    rem.firstSeen[i] = -1.0f;
    steer(c.puppet, m.pos);
    applyVitals(c.puppet.unit, m.health, m.maxHealth);
  }

  // Minions the stand-in summoned that nobody reports (one too many, or the real one is gone).
  for (auto it = rem.strays.begin(); it != rem.strays.end();)
  {
    bool still = false;
    for (engine::Unit* u : unclaimed)
      still = still || u == it->first;
    it = still ? std::next(it) : rem.strays.erase(it);
  }
  for (engine::Unit* u : unclaimed)
  {
    bool known = false;
    for (auto& s : rem.strays)
      known = known || s.first == u;
    if (!known)
      rem.strays.push_back({u, now});
  }
  for (auto it = rem.strays.begin(); it != rem.strays.end();)
  {
    if (now - it->second >= kStrayGrace)
    {
      engine::kill(it->first);
      TLMP_LOG("minions: slot %u's stand-in summoned %p that nobody reports: removed", slot, it->first);
      it = rem.strays.erase(it);
    }
    else
      ++it;
  }
}

void tick()
{
  if (!g_session || !g_session->isConnected())
    return;
  float now = clock();
  static float nextReport = 0.0f;
  if (now >= nextReport)
  {
    nextReport = now + kReportInterval;
    report();
  }
  engine::Level* level = currentLevel();
  if (!level)
    return;
  uint32_t here = levelId(level);
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
  {
    Remote& rem = g_remote[s];
    if (s == g_session->localSlot() || rem.levelId != here || !avatarOf(s))
      continue;
    show(s, rem, level, now);
  }
}

void onLevelDestroyed(void* level)
{
  for (Remote& rem : g_remote)
    for (Copy& c : rem.copies)
      if (c.puppet.level == level)
        c = {}; // freed with the level
  for (Remote& rem : g_remote)
    rem.strays.clear();
}

void attach(net::Session* session)
{
  g_session = session;
}

} // namespace tlmp::game::minions
