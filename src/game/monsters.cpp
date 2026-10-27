#include "game/monsters.h"
#include "game/combat.h"
#include "game/floor.h"
#include "game/savestate.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/internal.h"
#include "log.h"
#include "net/session.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tlmp::game::monsters {

using net::Msg;

constexpr float kRosterInterval = 1.0f;
constexpr float kStateInterval = 0.1f;
constexpr float kStateRadius = 80.0f;     // only stream monsters this close to a client

static net::Session* g_session = nullptr;
static bool g_allowKill = false; // set while we kill a mirrored monster on the host's word

// --- level enumeration ----------------------------------------------------------------------

static bool isCharacter(uintptr_t unit)
{
  const char* n = diag::rttiName(unit);
  return n && (std::strcmp(n, ".?AVCMonster@@") == 0);
}

static uintptr_t localPetUnit()
{
  uintptr_t client = gameClient();
  return client ? engine::field<uintptr_t>(reinterpret_cast<void*>(client), engine::kClientPet) : 0;
}

// Hostile monster in the sense of "the host runs it". GetAlignment alone isn't enough: it swaps
// 1 and 2 for units with tag 0x50, so player-side units (pets) can read as hostile. Anything
// whose own alignment is the player side, and every pet we know of, is excluded.
static bool isHostileMonster(engine::Unit* unit)
{
  if (!unit || !isCharacter(reinterpret_cast<uintptr_t>(unit)))
    return false;
  if (engine::field<int>(unit, engine::kUnitAlignment) == engine::kAlignmentPlayer)
    return false;
  if (reinterpret_cast<uintptr_t>(unit) == localPetUnit() || isAvatarUnit(unit))
    return false;
  return engine::isHostileAlignment(engine::GetAlignment(unit));
}


template <typename Fn>
static void forEachUnit(engine::Level* level, Fn&& fn)
{
  uint32_t list = 0, node = 0;
  if (!level || !diag::readU32(reinterpret_cast<uintptr_t>(level) + engine::kLevelUnits, list) || !list ||
      !diag::readU32(list, node))
    return;
  for (int guard = 0; node && guard < 20000; guard++)
  {
    uint32_t unit = 0, next = 0;
    diag::readU32(node, unit);
    diag::readU32(node + 4, next);
    if (unit)
      fn(reinterpret_cast<engine::Unit*>(unit));
    node = next;
  }
}

static int aiState(engine::Unit* unit)
{
  return engine::field<int>(unit, engine::kUnitAIState);
}

static bool isDead(engine::Unit* unit)
{
  int s = aiState(unit);
  return s == engine::kAIStateDying || s == engine::kAIStateDead;
}

// Host side: a monster counts as dead once it is dying/dead or out of health.
static bool hasDied(engine::Unit* unit)
{
  return isDead(unit) || engine::field<float>(unit, engine::kUnitHealth) <= 0.0f;
}

static uint64_t guidOf(engine::Unit* unit)
{
  return engine::field<uint64_t>(unit, engine::kUnitGuid);
}

static void writeVec(net::Writer& w, const Vec3& v)
{
  w.f32(v.x);
  w.f32(v.y);
  w.f32(v.z);
}

static Vec3 readVec(net::Reader& r)
{
  Vec3 v;
  v.x = r.f32();
  v.y = r.f32();
  v.z = r.f32();
  return v;
}

// Diagnostic: every non-hostile or pet-like monster in the level, every 15 s.
static void petCensus(engine::Level* level, const char* side)
{
  static float next = 0.0f;
  if (clock() < next)
    return;
  next = clock() + 15.0f;
  forEachUnit(level, [&](engine::Unit* u) {
    if (!isCharacter(reinterpret_cast<uintptr_t>(u)) || isHostileMonster(u))
      return;
    const char* what = reinterpret_cast<uintptr_t>(u) == localPetUnit() ? "my pet"
                       : isAvatarUnit(u)                               ? "remote player's pet"
                                                                        : "other";
    Vec3 p = unitPosition(u);
    TLMP_LOG("monsters: [%s census] %p %016llx raw alignment %d (GetAlignment %d) at (%.1f, %.1f): %s", side, u,
             guidOf(u), engine::field<int>(u, engine::kUnitAlignment), engine::GetAlignment(u), p.x, p.z, what);
  });
}

// --- a monster exactly as the game saves it (savestate.h) ----------------------------------------

static bool g_spawningPuppet = false; // our own AddUnit calls

// `scale`: the party health multiplier already in its health (travels with it, so whoever builds
// it knows not to apply it again).
static bool captureMonster(engine::Unit* u, std::vector<uint8_t>& out, float scale = 1.0f)
{
  void* state = savestate::create(engine::kCharStateSize, engine::CharStateCtor);
  if (!state)
    return false;
  engine::virtualFn<void(__thiscall*)(void*, void*)>(u, engine::kCharCaptureSlot)(u, state);
  bool ok = savestate::toBytes(state, engine::CharStateWrite, out);
  savestate::destroy(state);
  // The state doesn't carry the rolled maximum health (each new unit rolls its own): append it,
  // and the multiplier in it.
  int trailer[3] = {engine::field<int>(u, engine::kUnitBaseHealth), engine::field<int>(u, engine::kUnitBonusHealth), 0};
  std::memcpy(&trailer[2], &scale, sizeof(float));
  const auto* b = reinterpret_cast<const uint8_t*>(trailer);
  out.insert(out.end(), b, b + sizeof(trailer));
  return ok;
}

// Builds the monster a saved state describes (champion name and bonuses, equipment and all) and
// puts it in the level at `pos`, in the floor loader's order (0x413F84): create, AddUnit, restore.
static engine::Unit* createFromState(const std::vector<uint8_t>& bytes, engine::Level* level, const Vec3& pos,
                                     float* scale = nullptr)
{
  uintptr_t client = gameClient();
  void* resources = client ? engine::field<void*>(reinterpret_cast<void*>(client), engine::kClientResources) : nullptr;
  void* state = resources ? savestate::create(engine::kCharStateSize, engine::CharStateCtor) : nullptr;
  if (!state)
    return nullptr;
  engine::Unit* unit = nullptr;
  int base[3];
  if (bytes.size() <= sizeof(base))
  {
    savestate::destroy(state);
    return nullptr;
  }
  std::memcpy(base, bytes.data() + bytes.size() - sizeof(base), sizeof(base));
  if (scale)
    std::memcpy(scale, &base[2], sizeof(float));
  std::vector<uint8_t> saved(bytes.begin(), bytes.end() - sizeof(base));
  if (savestate::fromBytes(state, engine::CharStateRead, *reinterpret_cast<const uint32_t*>(engine::kCharStateVersion),
                           saved))
  {
    uint64_t guid = engine::field<uint64_t>(state, engine::kCharStateGuid);
    int lvl = engine::field<int>(state, engine::kCharStateLevel);
    unit = static_cast<engine::Unit*>(engine::CreateByGuid(resources, static_cast<uint32_t>(guid),
                                                           static_cast<uint32_t>(guid >> 32), lvl, 1, 0));
    if (unit && !isCharacter(reinterpret_cast<uintptr_t>(unit)))
    {
      engine::destroyUnit(unit);
      unit = nullptr;
    }
    if (unit)
    {
      g_spawningPuppet = true;
      engine::AddUnit(level, unit, &pos, false);
      g_spawningPuppet = false;
      engine::virtualFn<void(__thiscall*)(void*, void*)>(unit, engine::kCharRestoreSlot)(unit, state);
      *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(unit) + engine::kUnitBaseHealth) = base[0];
      *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(unit) + engine::kUnitBonusHealth) = base[1];
    }
  }
  savestate::destroy(state);
  return unit;
}

// --- host --------------------------------------------------------------------------------------

struct Tracked
{
  engine::Unit* unit = nullptr;
  uint64_t guid = 0;
  int level = 1;
  Vec3 spawn{};
  bool deathSent = false;
  float scale = 1.0f; // party health multiplier applied to it (see scaleNow)
};

static engine::Level* g_hostLevel = nullptr;
static uint32_t g_nextId = 1;
static std::unordered_map<uint32_t, Tracked> g_tracked;
static std::unordered_map<engine::Unit*, uint32_t> g_trackedIds;

// --- party size ---------------------------------------------------------------------------------
// Monsters on a floor get more health for every player in the game beyond the first (the host's
// setting, sent with the world seed; 0 = off). The owner applies it to its monsters and keeps it in
// step as players come and go (health keeps its share of the maximum); copies show the result.
static float g_healthPerPlayer = 0.5f;
static bool g_fullStateDue = false; // after a rescale: every copy needs its new maximum, near or far

void setHealthPerPlayer(float perPlayer)
{
  if (perPlayer < 0.0f)
    perPlayer = 0.0f;
  if (perPlayer != g_healthPerPlayer)
    TLMP_LOG("monsters: monster health +%.0f%% per extra player", perPlayer * 100.0f);
  g_healthPerPlayer = perPlayer;
}

static int playersInGame()
{
  int n = 0;
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
    n += floor::slotInGame(s) ? 1 : 0;
  return n < 1 ? 1 : n;
}

static float scaleNow()
{
  return 1.0f + g_healthPerPlayer * static_cast<float>(playersInGame() - 1);
}

// Max health is (base + bonus) scaled by stats, plus a flat stat: scaling base and bonus scales it.
static void rescale(Tracked& t, float to)
{
  if (!t.unit || to <= 0.0f || std::fabs(to - t.scale) < 0.001f)
    return;
  float f = to / t.scale;
  auto* base = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(t.unit) + engine::kUnitBaseHealth);
  auto* bonus = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(t.unit) + engine::kUnitBonusHealth);
  auto* health = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(t.unit) + engine::kUnitHealth);
  *base = static_cast<int>(std::lround(*base * f));
  *bonus = static_cast<int>(std::lround(*bonus * f));
  if (!isDead(t.unit))
    *health = *health * f;
  t.scale = to;
}
static float g_nextRoster = 0.0f;
static float g_nextState = 0.0f;
static uint32_t g_looksSent[net::kMaxPlayers] = {}; // level each slot has all looks for

static bool anyoneElseHere(uint32_t lid)
{
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
    if (s != g_session->localSlot() && floor::slotInGame(s) && floor::slotLevel(s) == lid)
      return true;
  return false;
}

static void writeLook(net::Writer& w, uint32_t lid, uint32_t id, const std::vector<uint8_t>& bytes)
{
  w.u32(lid);
  w.u32(id);
  w.bytes(bytes);
}

static void hostReset(engine::Level* level)
{
  g_hostLevel = level;
  g_tracked.clear();
  g_trackedIds.clear();
}

static void hostScan(engine::Level* level)
{
  std::unordered_set<engine::Unit*> present;
  forEachUnit(level, [&](engine::Unit* unit) {
    if (!isHostileMonster(unit))
      return;
    present.insert(unit);
    if (g_trackedIds.count(unit))
      return;
    uint32_t id = g_nextId++;
    g_trackedIds[unit] = id;
    g_tracked[id] = Tracked{unit, guidOf(unit), engine::field<int>(unit, engine::kUnitLevel), unitPosition(unit), false};
    rescale(g_tracked[id], scaleNow());
    // Others on the floor build their copy from this (champions look like champions).
    std::vector<uint8_t> look;
    if (anyoneElseHere(levelId(level)) && captureMonster(unit, look, g_tracked[id].scale))
    {
      net::Writer w(Msg::MonsterLook);
      writeLook(w, levelId(level), id, look);
      floor::sendToFloor(w, true);
    }
  });

  for (auto it = g_tracked.begin(); it != g_tracked.end();)
  {
    if (!present.count(it->second.unit))
    {
      // Gone from the level between two scans (some monsters vanish as they die): if nobody was
      // told it died, it did -- its copies go too, and players near it get their share.
      if (!it->second.deathSent)
      {
        TLMP_LOG("monsters: [owner] monster %u (%016llx) left the floor without dying here: reported dead", it->first,
                 it->second.guid);
        net::Writer w(Msg::MonsterDied);
        w.u32(levelId(level));
        w.u32(it->first);
        floor::sendToFloor(w, true);
      }
      g_trackedIds.erase(it->second.unit);
      it = g_tracked.erase(it);
      continue;
    }
    Tracked& t = it->second;
    if (!t.deathSent && hasDied(t.unit))
    {
      t.deathSent = true;
      TLMP_LOG("monsters: [owner] monster %u (%016llx) died (AI state %d, health %.1f)", it->first, t.guid,
               aiState(t.unit), engine::field<float>(t.unit, engine::kUnitHealth));
      net::Writer w(Msg::MonsterDied);
      w.u32(levelId(level));
      w.u32(it->first);
      floor::sendToFloor(w, true);
    }
    ++it;
  }
}

static void hostSendRoster(engine::Level* level)
{
  net::Writer w(Msg::MonsterRoster);
  w.u32(levelId(level));
  w.u16(static_cast<uint16_t>(g_tracked.size()));
  for (const auto& [id, t] : g_tracked)
  {
    w.u32(id);
    w.u32(static_cast<uint32_t>(t.guid));
    w.u32(static_cast<uint32_t>(t.guid >> 32));
    w.u16(static_cast<uint16_t>(t.level));
    w.u8(t.deathSent ? 1 : 0); // e.g. corpses restored with a saved floor: never shown alive
    writeVec(w, t.spawn);
  }
  floor::sendToFloor(w, true);
}

static void hostSendState(engine::Level* level)
{
  uint32_t lid = levelId(level);
  // Every few seconds the state of every monster goes out, not just of those near each player:
  // whatever a copy missed (a maximum that changed with the party size, a look that came late)
  // is put right within that time.
  static float nextFull = 0.0f;
  bool periodic = clock() >= nextFull;
  if (periodic)
  {
    nextFull = clock() + 5.0f;
    g_fullStateDue = g_fullStateDue || periodic;
  }
  for (uint8_t slot = 0; slot < net::kMaxPlayers; slot++)
  {
    if (slot == g_session->localSlot() || !floor::slotInGame(slot) || floor::slotLevel(slot) != lid)
      continue;
    const net::RemotePlayer& p = g_session->player(slot);
    Vec3 at{p.state.pos[0], p.state.pos[1], p.state.pos[2]};

    std::vector<uint32_t> ids;
    for (const auto& [id, t] : g_tracked)
      if (!t.deathSent && (g_fullStateDue || distXZ(unitPosition(t.unit), at) <= kStateRadius))
        ids.push_back(id);
    if (ids.empty())
      continue;

    net::Writer w(Msg::MonsterState);
    w.u32(lid);
    w.u16(static_cast<uint16_t>(ids.size()));
    for (uint32_t id : ids)
    {
      engine::Unit* u = g_tracked[id].unit;
      w.u32(id);
      writeVec(w, unitPosition(u));
      w.f32(engine::field<float>(u, engine::kUnitHealth));
      w.u32(static_cast<uint32_t>(engine::GetMaxHealth(u)));
      w.f32(g_tracked[id].scale);
      w.u8(static_cast<uint8_t>(aiState(u)));
    }
    floor::sendTo(slot, w, g_fullStateDue); // the full one must arrive
  }
  g_fullStateDue = false;
}

static void report(const char* fmt, ...);

static void hostTick()
{
  engine::Level* level = currentLevel();
  if (level)
    petCensus(level, "host");
  report("monsters: [owner] running %u hostile monsters", static_cast<unsigned>(g_tracked.size()));
  if (level != g_hostLevel)
    hostReset(level);
  if (!level)
    return;

  float now = clock();
  static float nextScale = 0.0f;
  if (now >= nextScale)
  {
    nextScale = now + 1.0f;
    float to = scaleNow();
    unsigned changed = 0;
    for (auto& [id, t] : g_tracked)
    {
      if (t.deathSent || std::fabs(t.scale - to) < 0.001f)
        continue;
      rescale(t, to);
      changed++;
    }
    if (changed)
    {
      g_fullStateDue = true;
      TLMP_LOG("monsters: [owner] %d player(s) in the game: monster health x%.2f (%u monsters adjusted)",
               playersInGame(), to, changed);
    }
  }
  if (now >= g_nextState)
  {
    g_nextState = now + kStateInterval;
    hostScan(level);
    hostSendState(level);
    // Players who just arrived get the look of every monster here, before the roster spawns copies.
    uint32_t lid = levelId(level);
    for (uint8_t slot = 0; slot < net::kMaxPlayers; slot++)
    {
      if (slot == g_session->localSlot() || !floor::slotInGame(slot) || floor::slotLevel(slot) != lid ||
          g_looksSent[slot] == lid)
        continue;
      g_looksSent[slot] = lid;
      unsigned sent = 0;
      for (const auto& [id, t] : g_tracked)
      {
        std::vector<uint8_t> look;
        if (t.deathSent || hasDied(t.unit) || !captureMonster(t.unit, look, t.scale))
          continue;
        net::Writer w(Msg::MonsterLook);
        writeLook(w, lid, id, look);
        floor::sendTo(slot, w, true);
        sent++;
      }
      TLMP_LOG("monsters: sent slot %u the looks of %u monsters", slot, sent);
      // A newcomer may hold copies made otherwise (e.g. kept from running the floor itself for a
      // moment): the next state goes out for every monster, with the maxima.
      g_fullStateDue = true;
    }
  }
  if (now >= g_nextRoster)
  {
    g_nextRoster = now + kRosterInterval;
    hostSendRoster(level);
  }
}

// --- client ------------------------------------------------------------------------------------
// The client never creates hostile monsters of its own: AddUnit refuses them (spawners and
// level population still run, their monsters just never enter the level), and every hostile
// monster on screen is a puppet of one of the host's, created from the roster.

struct Mirror
{
  uint64_t guid = 0;
  int level = 1;
  Vec3 hostSpawn{};
  Puppet puppet;
  bool spawnFailed = false;

  bool hasState = false;
  Vec3 target{};
  float health = 0.0f;
  uint32_t maxHealth = 0; // the owner's (0 = not heard yet)
  float scale = 1.0f;     // party health multiplier the owner applied
  bool dead = false;

  std::vector<uint8_t> look; // the monster as the owner's game saves it
  float lookDeadline = 0.0f; // build from the GUID alone if no look arrived by then
  bool fromLook = false;     // the copy was built from the look (else from the type only)
};
// Looks that came before their roster entry (or while we still thought we ran the floor), by level
// and id: (levelId << 32) | id.
static std::unordered_map<uint64_t, std::vector<uint8_t>> g_earlyLooks;

static uint64_t lookKey(uint32_t lid, uint32_t id)
{
  return (static_cast<uint64_t>(lid) << 32) | id;
}
constexpr float kLookWait = 2.0f;

static engine::Level* g_clientLevel = nullptr;
static bool g_hostHere = false; // the host is on our floor
static uint32_t g_rosterLevel = 0;
static bool g_haveRoster = false;
static std::unordered_map<uint32_t, Mirror> g_mirrors;
static std::unordered_set<engine::Unit*> g_puppets;
static bool g_destroyingOurs = false; // our own destroyUnit calls (not news for the logs)
static unsigned g_refused = 0;

static void clientReset(engine::Level* level)
{
  // Early looks for other floors are of no use any more.
  uint32_t lid = levelId(level);
  for (auto it = g_earlyLooks.begin(); it != g_earlyLooks.end();)
    it = static_cast<uint32_t>(it->first >> 32) == lid ? std::next(it) : g_earlyLooks.erase(it);
  g_clientLevel = level;
  g_hostHere = false;
  g_mirrors.clear();
  g_puppets.clear();
}

static void spawnPuppet(uint32_t id, Mirror& m, engine::Level* level)
{
  Vec3 pos = m.hasState ? m.target : m.hostSpawn;
  engine::Unit* unit = nullptr;
  if (!m.look.empty())
  {
    unit = createFromState(m.look, level, pos, &m.scale);
    m.look.clear(); // used (and never again: a respawned copy would be a new id)
    m.fromLook = unit != nullptr;
    if (!unit)
      TLMP_LOG("monsters: could not build %016llx (id %u) from its look; using its type", m.guid, id);
  }
  else if (clock() < m.lookDeadline)
    return; // its look is probably on the way
  if (!unit)
  {
    void* factory = engine::field<void*>(level, engine::kLevelUnitFactory);
    unit = factory ? engine::createMonsterByGuid(factory, m.guid, m.level) : nullptr;
    if (!unit)
    {
      TLMP_LOG("monsters: cannot create GUID %016llx (id %u)", m.guid, id);
      m.spawnFailed = true;
      return;
    }
    g_spawningPuppet = true;
    engine::AddUnit(level, unit, &pos, false);
    g_spawningPuppet = false;
  }
  m.puppet = Puppet{unit, level, pos, clock()};
  g_puppets.insert(unit);
}

static void removePuppet(Mirror& m)
{
  if (!m.puppet.unit)
    return;
  g_puppets.erase(m.puppet.unit);
  if (m.puppet.level)
  {
    engine::RemoveUnit(m.puppet.level, m.puppet.unit, false);
    g_destroyingOurs = true;
    engine::destroyUnit(m.puppet.unit);
    g_destroyingOurs = false;
  }
  m.puppet = {};
}

// `killer` (our own player, when we share in the kill) makes the game drop our own loot for it and
// fire the kill event our quests count; without one the copy just dies.
static void killMirror(Mirror& m, engine::Unit* killer = nullptr)
{
  if (!m.puppet.unit || isDead(m.puppet.unit))
    return;
  g_allowKill = true;
  engine::kill(m.puppet.unit, killer);
  g_allowKill = false;
  TLMP_LOG("monsters: [client] killed copy %p (%016llx)%s: AI state now %d", m.puppet.unit, m.guid,
           killer ? " as my kill" : "", aiState(m.puppet.unit));
}

// The client never owns hostile monsters. Any in the level that isn't one of our copies got in
// some other way (e.g. restored from a floor saved while copies were on it, which bypasses
// AddUnit) and is taken out of the level and out of sight.
static void sweepForeignMonsters(engine::Level* level)
{
  static float next = 0.0f;
  if (clock() < next)
    return;
  next = clock() + 1.0f;
  std::vector<engine::Unit*> foreign;
  forEachUnit(level, [&](engine::Unit* u) {
    if (isHostileMonster(u) && !g_puppets.count(u))
      foreign.push_back(u);
  });
  for (engine::Unit* u : foreign)
  {
    TLMP_LOG("monsters: removed a local-only monster %p (%016llx) that bypassed spawning", u, guidOf(u));
    engine::RemoveUnit(level, u, false);
    Vec3 p = unitPosition(u);
    p.y -= 1000.0f;
    engine::setPosition(u, p);
  }
}

static void clientTick()
{
  engine::Level* level = currentLevel();
  if (level != g_clientLevel)
    clientReset(level);
  if (!level)
    return;
  petCensus(level, "client");

  {
    unsigned alive = 0, dead = 0;
    for (const auto& [id, m] : g_mirrors)
    {
      alive += m.puppet.unit && !m.dead;
      dead += m.dead;
    }
    report("monsters: [copies] owner here %d, its monsters %u (%u shown alive, %u dead); refused %u local spawns",
           g_hostHere, static_cast<unsigned>(g_mirrors.size()), alive, dead, g_refused);
  }

  bool sameLevel = g_haveRoster && g_rosterLevel == levelId(level);
  if (sameLevel != g_hostHere)
    TLMP_LOG("monsters: host %s this floor", sameLevel ? "is on" : "left");
  g_hostHere = sameLevel;
  if (!sameLevel)
    return;
  sweepForeignMonsters(level);

  for (auto& [id, m] : g_mirrors)
  {
    if (!m.puppet.unit && !m.dead && !m.spawnFailed)
      spawnPuppet(id, m, level);
    if (!m.puppet.unit || isDead(m.puppet.unit))
      continue;
    if (m.dead)
    {
      killMirror(m);
      continue;
    }
    if (!m.hasState)
      continue;
    if (m.maxHealth)
      applyVitals(m.puppet.unit, m.health, m.maxHealth); // the owner's maximum too (party size)
    else
      *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(m.puppet.unit) + engine::kUnitHealth) = m.health;
    steer(m.puppet, m.target);
  }
}

static void onRoster(net::Reader& r)
{
  // Settle onto the current floor first: a reset after this (from clientTick noticing the level, e.g.
  // right after we stopped running it) would throw away the copies and looks this roster sets up.
  if (engine::Level* cur = currentLevel(); cur != g_clientLevel)
    clientReset(cur);
  uint32_t lid = r.u32();
  uint16_t count = r.u16();
  if (lid != g_rosterLevel || !g_haveRoster)
  {
    // Host is on a (new) floor: forget what we showed for the old one.
    for (auto& [id, m] : g_mirrors)
      removePuppet(m);
    g_mirrors.clear();
    g_rosterLevel = lid;
    g_haveRoster = true;
    TLMP_LOG("monsters: host roster for level %08x (%u monsters)", lid, count);
  }

  std::unordered_set<uint32_t> ids;
  for (uint16_t i = 0; i < count && r.ok(); i++)
  {
    uint32_t id = r.u32();
    uint64_t guid = r.u32();
    guid |= static_cast<uint64_t>(r.u32()) << 32;
    int level = r.u16();
    bool dead = r.u8() != 0;
    Vec3 spawn = readVec(r);
    if (!r.ok())
      return;
    ids.insert(id);
    auto [it, inserted] = g_mirrors.try_emplace(id);
    if (inserted)
    {
      it->second.guid = guid;
      it->second.level = level;
      it->second.hostSpawn = spawn;
      it->second.lookDeadline = clock() + kLookWait;
      if (auto early = g_earlyLooks.find(lookKey(lid, id)); early != g_earlyLooks.end())
      {
        it->second.look = std::move(early->second);
        g_earlyLooks.erase(early);
      }
    }
    if (dead && !it->second.dead)
    {
      it->second.dead = true; // never spawned; an existing copy is killed in clientTick
    }
  }

  // Host monsters that are gone (corpse cleanup, despawn).
  for (auto it = g_mirrors.begin(); it != g_mirrors.end();)
  {
    if (ids.count(it->first))
    {
      ++it;
      continue;
    }
    removePuppet(it->second);
    it = g_mirrors.erase(it);
  }
}

static void onState(net::Reader& r)
{
  uint32_t lid = r.u32();
  uint16_t count = r.u16();
  if (!g_haveRoster || lid != g_rosterLevel)
    return;
  for (uint16_t i = 0; i < count && r.ok(); i++)
  {
    uint32_t id = r.u32();
    Vec3 pos = readVec(r);
    float health = r.f32();
    uint32_t maxHealth = r.u32();
    float scale = r.f32();
    uint8_t ai = r.u8();
    if (!r.ok())
      break;
    auto it = g_mirrors.find(id);
    if (it == g_mirrors.end())
      continue;
    it->second.hasState = true;
    it->second.target = pos;
    it->second.health = health;
    it->second.maxHealth = maxHealth;
    it->second.scale = scale;
    if (ai == engine::kAIStateDying || ai == engine::kAIStateDead)
      it->second.dead = true;
  }
}

static void onLook(net::Reader& r)
{
  uint32_t lid = r.u32();
  uint32_t id = r.u32();
  std::vector<uint8_t> look = r.bytes();
  if (!r.ok() || lid != levelId(currentLevel()))
    return;
  auto it = g_mirrors.find(id);
  if (it == g_mirrors.end())
  {
    if (g_earlyLooks.size() < 4096)
      g_earlyLooks[lookKey(lid, id)] = std::move(look);
    return;
  }
  if (floor::ownsCurrentLevel())
    return;
  Mirror& m = it->second;
  if (m.puppet.unit && !m.fromLook && !m.dead)
  {
    // Built from the type alone before its look came: rebuild it as the owner has it.
    removePuppet(m);
    m.spawnFailed = false;
  }
  if (!m.puppet.unit)
    m.look = std::move(look);
}

static void onDied(net::Reader& r)
{
  uint32_t lid = r.u32();
  uint32_t id = r.u32();
  if (!r.ok() || !g_haveRoster || lid != g_rosterLevel)
    return;
  auto it = g_mirrors.find(id);
  if (it == g_mirrors.end())
  {
    TLMP_LOG("monsters: [client] host monster %u died but we have no copy of it", id);
    return;
  }
  TLMP_LOG("monsters: [client] host monster %u died (copy %p)", id, it->second.puppet.unit);
  bool mine = it->second.puppet.unit && !it->second.dead && !isDead(it->second.puppet.unit) &&
              combat::onHostKill(it->second.puppet.unit); // our share of the XP, before the copy dies
  it->second.dead = true;
  killMirror(it->second, mine ? reinterpret_cast<engine::Unit*>(localPlayer()) : nullptr);
}

// Logs at most every 5 s.
static void report(const char* fmt, ...)
{
  static float next = 0.0f;
  if (clock() < next)
    return;
  next = clock() + 5.0f;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  TLMP_LOG("%s", buf);
}

// --- public ------------------------------------------------------------------------------------

void attach(net::Session* session)
{
  g_session = session;
}

// "Client" here means: on a floor some other machine runs (see floor.h), so hostile monsters
// are copies of that machine's.
static bool isClient()
{
  return g_session && g_session->isConnected() && !floor::ownsCurrentLevel();
}

static bool isPuppet(void* unit)
{
  return g_puppets.count(static_cast<engine::Unit*>(unit)) != 0;
}

void tick()
{
  if (!g_session || !g_session->isConnected())
    return;
  if (floor::ownsCurrentLevel())
    hostTick();
  else
    clientTick();
}

void onMessage(uint8_t, Msg type, net::Reader& r)
{
  if (type == Msg::MonsterLook)
  {
    onLook(r); // kept even while we (still) run the floor: we may hand it over any moment
    return;
  }
  if (!isClient())
    return;
  switch (type)
  {
    case Msg::MonsterRoster:
      onRoster(r);
      break;
    case Msg::MonsterState:
      onState(r);
      break;
    case Msg::MonsterDied:
      onDied(r);
      break;
    case Msg::MonsterLook:
      onLook(r);
      break;
    default:
      break;
  }
}

void beforeSave()
{
  if (!isClient())
    return;
  unsigned removed = 0;
  for (auto& [id, m] : g_mirrors)
  {
    if (m.puppet.unit)
    {
      removePuppet(m); // living ones are recreated from the roster on the next tick
      removed++;
    }
  }
  if (removed)
    TLMP_LOG("monsters: removed %u monster copies before the game saves", removed);
}

void onLevelDestroyed(void* level)
{
  if (level == g_hostLevel)
    hostReset(nullptr);
  if (level == g_clientLevel)
    clientReset(nullptr); // the level frees the puppets it owns
}

bool allowAddUnit(void* level, void* unit, uintptr_t caller)
{
  auto* u = static_cast<engine::Unit*>(unit);
  floor::onLevelLoading(level); // decides who runs a level that is just being built
  if (!g_session || !g_session->isConnected() || g_spawningPuppet)
    return true;
  if (isCharacter(reinterpret_cast<uintptr_t>(u)) && !isHostileMonster(u) && !isAvatarUnit(u))
  {
    // Diagnostic: friendly monsters (pets, NPCs) the game adds itself, and who adds them.
    TLMP_LOG("monsters: game added friendly %p %016llx (raw alignment %d) from %08x", u, guidOf(u),
             engine::field<int>(u, engine::kUnitAlignment), static_cast<unsigned>(caller));
  }
  if (floor::ownsLevel(level) || !isHostileMonster(u))
    return true;
  g_refused++;
  // Never enters the level, but its model may already be in the scene: move it far out of
  // sight so no frozen copy is left standing around.
  Vec3 p = unitPosition(u);
  p.y -= 1000.0f;
  engine::setPosition(u, p);
  return false;
}

bool shouldSkipAI(void* monster)
{
  // Remote players' pets are moved only by their owner's state; left to their own AI they go
  // after nearby monsters and ignore our movement orders.
  if (isAvatarUnit(monster))
    return true;
  // Puppets are moved by the host's state, never by their own AI. Dying ones keep updating:
  // UpdateAI's dying state is what finishes the death animation.
  return isClient() && isPuppet(monster) && !isDead(static_cast<engine::Unit*>(monster));
}

bool shouldBlockKill(void* unit)
{
  if (isAvatarUnit(unit))
    return !killingStandIn(); // remote players' avatars only mirror; their real owner decides
  return isClient() && !g_allowKill && isPuppet(unit); // only the host decides when they die
}

uint32_t puppetId(const void* unit)
{
  if (!unit || !isPuppet(const_cast<void*>(unit)))
    return 0;
  for (const auto& [id, m] : g_mirrors)
    if (m.puppet.unit == unit)
      return id;
  return 0;
}

engine::Unit* puppetById(uint32_t id)
{
  auto it = g_mirrors.find(id);
  return it != g_mirrors.end() ? it->second.puppet.unit : nullptr;
}

bool isPuppetUnit(const void* unit)
{
  return isClient() && isPuppet(const_cast<void*>(unit));
}

uint32_t trackedId(const void* unit)
{
  auto it = g_trackedIds.find(const_cast<engine::Unit*>(unit));
  return it != g_trackedIds.end() ? it->second : 0;
}

uint32_t trackedIdByAnimation(const void* animObject)
{
  for (const auto& [id, t] : g_tracked)
    if (!t.deathSent && engine::field<const void*>(t.unit, engine::kUnitAnimation) == animObject)
      return hasDied(t.unit) ? 0 : id; // its death animation plays on the copy from Kill
  return 0;
}

engine::Unit* trackedById(uint32_t id)
{
  auto it = g_tracked.find(id);
  if (it == g_tracked.end() || !g_hostLevel)
    return nullptr;
  // The table is refreshed every 0.1 s; a monster the level freed since then must not be used.
  bool present = false;
  forEachUnit(g_hostLevel, [&](engine::Unit* u) { present = present || u == it->second.unit; });
  return present ? it->second.unit : nullptr;
}

// --- floor handover (floor.cpp) -----------------------------------------------------------------

void writeSnapshot(net::Writer& w)
{
  w.u32(g_nextId);
  uint16_t n = 0;
  for (const auto& [id, t] : g_tracked)
    n += t.unit ? 1 : 0;
  w.u16(n);
  for (const auto& [id, t] : g_tracked)
  {
    if (!t.unit)
      continue;
    bool dead = t.deathSent || hasDied(t.unit);
    w.u32(id);
    w.u32(static_cast<uint32_t>(t.guid));
    w.u32(static_cast<uint32_t>(t.guid >> 32));
    w.u16(static_cast<uint16_t>(t.level));
    w.u8(dead ? 1 : 0);
    writeVec(w, unitPosition(t.unit));
    w.f32(engine::field<float>(t.unit, engine::kUnitHealth));
    w.f32(t.scale);
    std::vector<uint8_t> state;
    if (!dead)
      captureMonster(t.unit, state, t.scale);
    w.bytes(state);
  }
}

// Takes a monster out of the level the way refused spawns are: out of the lists, out of sight.
static void retire(engine::Level* level, engine::Unit* u)
{
  engine::RemoveUnit(level, u, false);
  Vec3 p = unitPosition(u);
  p.y -= 1000.0f;
  engine::setPosition(u, p);
}

void becomeOwner(net::Reader* r)
{
  engine::Level* level = currentLevel();
  if (!level)
    return;
  struct Entry
  {
    uint64_t guid;
    int level;
    bool dead;
    Vec3 pos;
    float health;
    float scale;
    std::vector<uint8_t> state;
  };
  std::unordered_map<uint32_t, Entry> snap;
  uint32_t snapNext = 0;
  if (r)
  {
    snapNext = r->u32();
    uint16_t n = r->u16();
    for (uint16_t i = 0; i < n && r->ok(); i++)
    {
      uint32_t id = r->u32();
      Entry e;
      e.guid = r->u32();
      e.guid |= static_cast<uint64_t>(r->u32()) << 32;
      e.level = r->u16();
      e.dead = r->u8() != 0;
      e.pos = readVec(*r);
      e.health = r->f32();
      e.scale = r->f32();
      e.state = r->bytes();
      if (r->ok())
        snap[id] = std::move(e);
    }
  }

  // Our copies become the real monsters, under the same ids.
  std::unordered_map<uint32_t, Tracked> tracked;
  std::unordered_set<engine::Unit*> kept;
  for (auto& [id, m] : g_mirrors)
  {
    if (!m.puppet.unit)
      continue;
    kept.insert(m.puppet.unit);
    tracked[id] = Tracked{m.puppet.unit, m.guid, m.level, m.hostSpawn, m.dead || isDead(m.puppet.unit), m.scale};
  }
  g_mirrors.clear();
  g_puppets.clear();
  if (r)
  {
    // The snapshot is the floor: anything else here (e.g. from this machine's own floor save) goes.
    std::vector<engine::Unit*> others;
    forEachUnit(level, [&](engine::Unit* u) {
      if (isHostileMonster(u) && !kept.count(u))
        others.push_back(u);
    });
    for (engine::Unit* u : others)
      retire(level, u);
    if (!others.empty())
      TLMP_LOG("monsters: took %u of my own monsters off the floor for the snapshot's",
               static_cast<unsigned>(others.size()));
  }
  else
  {
    for (auto& [id, t] : g_tracked)
      if (!tracked.count(id))
        tracked[id] = t;
  }

  unsigned spawned = 0, killed = 0;
  for (auto& [id, e] : snap)
  {
    auto it = tracked.find(id);
    if (it != tracked.end())
    {
      engine::Unit* u = it->second.unit;
      if (e.dead && !isDead(u))
      {
        g_allowKill = true;
        engine::kill(u);
        g_allowKill = false;
        killed++;
      }
      else if (!e.dead)
      {
        // The snapshot's maximum (its state's trailer: base, bonus, multiplier) with its health: a
        // copy kept here may have been built from the type alone, with a roll of its own.
        int trailer[3];
        if (e.state.size() > sizeof(trailer))
        {
          std::memcpy(trailer, e.state.data() + e.state.size() - sizeof(trailer), sizeof(trailer));
          *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(u) + engine::kUnitBaseHealth) = trailer[0];
          *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(u) + engine::kUnitBonusHealth) = trailer[1];
          std::memcpy(&it->second.scale, &trailer[2], sizeof(float));
        }
        *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(u) + engine::kUnitHealth) = e.health;
        if (distXZ(unitPosition(u), e.pos) > 1.5f)
          engine::setPosition(u, e.pos);
      }
      it->second.deathSent = it->second.deathSent || e.dead;
      continue;
    }
    if (e.dead)
      continue; // gone for good; nobody shows a corpse it never saw
    engine::Unit* u = e.state.empty() ? nullptr : createFromState(e.state, level, e.pos);
    if (!u)
    {
      void* factory = engine::field<void*>(level, engine::kLevelUnitFactory);
      u = factory ? engine::createMonsterByGuid(factory, e.guid, e.level) : nullptr;
      if (!u)
        continue;
      g_spawningPuppet = true;
      engine::AddUnit(level, u, &e.pos, false);
      g_spawningPuppet = false;
    }
    *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(u) + engine::kUnitHealth) = e.health;
    tracked[id] = Tracked{u, e.guid, e.level, e.pos, false, e.scale}; // its state carries the scaled health
    spawned++;
  }

  g_tracked = std::move(tracked);
  g_trackedIds.clear();
  uint32_t maxId = 0;
  for (const auto& [id, t] : g_tracked)
  {
    g_trackedIds[t.unit] = id;
    maxId = id > maxId ? id : maxId;
  }
  g_nextId = std::max({g_nextId, snapNext, maxId + 1});
  g_hostLevel = level;
  g_nextRoster = 0.0f; // tell the others right away
  for (uint8_t slot = 0; slot < net::kMaxPlayers; slot++)
    g_looksSent[slot] = 0; // everyone here gets the looks again (their copies may be type-only ones)
  TLMP_LOG("monsters: I run this floor's %u monsters now (%u created from the snapshot, %u killed per it)",
           static_cast<unsigned>(g_tracked.size()), spawned, killed);
}

void becomeMirror()
{
  engine::Level* level = currentLevel();
  for (auto& [id, t] : g_tracked)
  {
    if (!t.unit)
      continue;
    Mirror m;
    m.guid = t.guid;
    m.level = t.level;
    m.hostSpawn = t.spawn;
    m.puppet = Puppet{t.unit, level, unitPosition(t.unit), clock()};
    m.dead = t.deathSent || hasDied(t.unit);
    m.hasState = true;
    m.target = unitPosition(t.unit);
    m.health = engine::field<float>(t.unit, engine::kUnitHealth);
    g_mirrors[id] = m;
    g_puppets.insert(t.unit);
  }
  TLMP_LOG("monsters: the floor's %u monsters are copies now", static_cast<unsigned>(g_mirrors.size()));
  g_tracked.clear();
  g_trackedIds.clear();
  g_hostLevel = nullptr;
  g_clientLevel = level;
  g_rosterLevel = levelId(level);
  g_haveRoster = true;
  g_hostHere = true;
}

void forEachKnown(void (*fn)(uint32_t id, engine::Unit* unit, bool copy, bool dead, void* ctx), void* ctx)
{
  for (const auto& [id, t] : g_tracked)
    if (t.unit)
      fn(id, t.unit, false, t.deathSent || hasDied(t.unit), ctx);
  for (const auto& [id, m] : g_mirrors)
    if (m.puppet.unit)
      fn(id, m.puppet.unit, true, m.dead || isDead(m.puppet.unit), ctx);
}

void discardOwn()
{
  engine::Level* level = currentLevel();
  unsigned n = 0;
  for (auto& [id, t] : g_tracked)
    if (t.unit && level)
    {
      retire(level, t.unit);
      n++;
    }
  g_tracked.clear();
  g_trackedIds.clear();
  g_hostLevel = nullptr;
  TLMP_LOG("monsters: dropped my own %u monsters; this floor is run elsewhere", n);
}

bool scaleRange(float* lo, float* hi)
{
  bool any = false;
  for (const auto& [id, t] : g_tracked)
  {
    if (!t.unit || t.deathSent)
      continue;
    *lo = any ? std::min(*lo, t.scale) : t.scale;
    *hi = any ? std::max(*hi, t.scale) : t.scale;
    any = true;
  }
  return any;
}

bool copyScaleRange(float* lo, float* hi)
{
  bool any = false;
  for (const auto& [id, m] : g_mirrors)
  {
    if (!m.puppet.unit || m.dead)
      continue;
    *lo = any ? std::min(*lo, m.scale) : m.scale;
    *hi = any ? std::max(*hi, m.scale) : m.scale;
    any = true;
  }
  return any;
}

unsigned copiesWithoutLook()
{
  unsigned n = 0;
  for (const auto& [id, m] : g_mirrors)
    if (m.puppet.unit && !m.fromLook)
      n++;
  return n;
}

void onUnitDestroyed(void* unit)
{
  auto* u = static_cast<engine::Unit*>(unit);
  if (g_puppets.erase(u))
  {
    for (auto& [id, m] : g_mirrors)
      if (m.puppet.unit == u)
      {
        if (!g_destroyingOurs)
          TLMP_LOG("monsters: the game freed copy %u (%p, %s)", id, unit, m.dead ? "dead" : "alive");
        m.puppet = {}; // a dead one stays dead (not respawned); a living one comes back next tick
      }
  }
  auto it = g_trackedIds.find(u);
  if (it != g_trackedIds.end())
  {
    uint32_t id = it->second;
    auto t = g_tracked.find(id);
    if (t != g_tracked.end() && !t->second.deathSent && g_session && g_session->isConnected())
    {
      // Freed without dying here first: for everyone else it's gone too.
      net::Writer w(Msg::MonsterDied);
      w.u32(levelId(currentLevel()));
      w.u32(id);
      floor::sendToFloor(w, true);
    }
    if (!g_destroyingOurs)
      TLMP_LOG("monsters: the game freed monster %u (%p)", id, unit);
    if (t != g_tracked.end())
      g_tracked.erase(t);
    g_trackedIds.erase(it);
  }
}

void allowLocalSpawns(bool allow)
{
  g_spawningPuppet = allow;
}

bool isMonster(const void* unit)
{
  return unit && isCharacter(reinterpret_cast<uintptr_t>(unit));
}

bool captureState(engine::Unit* unit, std::vector<uint8_t>& out)
{
  return captureMonster(unit, out);
}

void retireUnit(engine::Level* level, engine::Unit* unit)
{
  retire(level, unit);
}

engine::Unit* spawnFromState(const std::vector<uint8_t>& state, const Vec3& pos)
{
  engine::Level* level = currentLevel();
  return level ? createFromState(state, level, pos) : nullptr;
}

bool otherLivingWithQuestTag(const void* except, uint64_t guid, uint32_t index)
{
  if (guid == ~0ull)
    return false;
  bool found = false;
  forEachUnit(currentLevel(), [&](engine::Unit* u) {
    if (u != except && isCharacter(reinterpret_cast<uintptr_t>(u)) && !hasDied(u) &&
        engine::field<uint64_t>(u, engine::kUnitQuestGuid) == guid &&
        engine::field<uint32_t>(u, engine::kUnitQuestIndex) == index)
      found = true;
  });
  return found;
}

} // namespace tlmp::game::monsters
