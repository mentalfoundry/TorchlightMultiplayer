#include "game/triggers.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/floor.h"
#include "game/internal.h"
#include "game/party.h"
#include "log.h"
#include "net/session.h"

#include <cstring>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace tlmp::game::triggers {

using net::Msg;

constexpr float kRosterInterval = 2.0f;
constexpr float kMatchDistance = 1.0f;

static net::Session* g_session = nullptr;
static bool g_replaying = false; // set while we drive a trigger on a remote player's behalf
static float g_nextRoster = 0.0f;

static int stateOf(engine::Unit* t)
{
  return engine::field<int>(t, engine::kTriggerState);
}

// Every live CTriggerUnit, recorded by the init/destructor hooks: the level doesn't keep them in
// its unit lists, so this is how they're found. Only the current floor's are alive at a time.
static std::unordered_set<engine::Unit*> g_registry;

void onTriggerCreated(void* trigger)
{
  g_registry.insert(static_cast<engine::Unit*>(trigger));
}

void onTriggerDestroyed(void* trigger)
{
  g_registry.erase(static_cast<engine::Unit*>(trigger));
}

template <typename Fn>
static void forEachTrigger(engine::Level*, Fn&& fn)
{
  for (engine::Unit* t : g_registry)
    fn(t);
}

static engine::Unit* findTrigger(engine::Level* level, uint64_t guid, float x, float z)
{
  engine::Unit* best = nullptr;
  float bestDist = kMatchDistance;
  forEachTrigger(level, [&](engine::Unit* t) {
    if (engine::field<uint64_t>(t, engine::kUnitGuid) != guid)
      return;
    float d = distXZ(unitPosition(t), Vec3{x, 0.0f, z});
    if (d <= bestDist)
    {
      best = t;
      bestDist = d;
    }
  });
  return best;
}

// Steps the local trigger forward with the game's Activate until it shows `state`.
static void applyState(uint64_t guid, float x, float z, int state)
{
  engine::Level* level = currentLevel();
  engine::Unit* t = findTrigger(level, guid, x, z);
  if (!t)
  {
    int total = 0, sameGuid = 0;
    float nearest = 1e9f;
    forEachTrigger(level, [&](engine::Unit* u) {
      total++;
      if (engine::field<uint64_t>(u, engine::kUnitGuid) == guid)
      {
        sameGuid++;
        float d = distXZ(unitPosition(u), Vec3{x, 0.0f, z});
        nearest = d < nearest ? d : nearest;
      }
    });
    TLMP_LOG("triggers: no local trigger %016llx near (%.1f, %.1f) for state %d (%d triggers here, %d with that "
             "GUID, nearest %.1f away)",
             guid, x, z, state, total, sameGuid, sameGuid ? nearest : -1.0f);
    return;
  }
  int maxState = engine::field<int>(t, engine::kTriggerMaxState);
  if (stateOf(t) != state)
    TLMP_LOG("triggers: replaying %016llx at (%.1f, %.1f): state %d -> %d (max %d, interactable %d)", guid, x, z,
             stateOf(t), state, maxState, engine::field<uint8_t>(t, engine::kTriggerInteractable));
  if (state < 0 || state > maxState + 1)
    return;
  g_replaying = true;
  for (int guard = 0; guard <= maxState + 1 && stateOf(t) != state; guard++)
  {
    if (!engine::field<uint8_t>(t, engine::kTriggerInteractable))
      break; // finished (e.g. an opened door can't be used again)
    int before = stateOf(t);
    engine::TriggerActivate(t, nullptr);
    if (stateOf(t) == before)
      break;
  }
  g_replaying = false;
  if (stateOf(t) != state)
    TLMP_LOG("triggers: %016llx stopped at state %d (wanted %d)", guid, stateOf(t), state);
}

static void writeUse(net::Writer& w, uint32_t lid, uint64_t guid, float x, float z, int state)
{
  w.u32(lid);
  w.u32(static_cast<uint32_t>(guid));
  w.u32(static_cast<uint32_t>(guid >> 32));
  w.f32(x);
  w.f32(z);
  w.u32(static_cast<uint32_t>(state));
}

bool isReplaying()
{
  return g_replaying;
}

// To everyone: the host sends directly; a client sends to the host, which passes it on.
static void broadcast(const net::Writer& w)
{
  if (g_session->isHost())
    g_session->sendToAll(w, true);
  else
    g_session->sendTo(0, w, true);
}

void attach(net::Session* session)
{
  g_session = session;
}

int beforeInteract(void* trigger, void*)
{
  return stateOf(static_cast<engine::Unit*>(trigger));
}

void afterInteract(void* trigger, void* user, int stateBefore)
{
  auto* t = static_cast<engine::Unit*>(trigger);
  if (g_replaying || !g_session || !g_session->isConnected() || stateOf(t) == stateBefore)
    return;
  if (user != reinterpret_cast<void*>(localPlayer()))
    return; // only the player sitting at this machine reports their own actions
  Vec3 p = unitPosition(t);
  uint64_t guid = engine::field<uint64_t>(t, engine::kUnitGuid);
  TLMP_LOG("triggers: used %016llx at (%.1f, %.1f) -> state %d", guid, p.x, p.z, stateOf(t));
  net::Writer w(Msg::TriggerUse);
  writeUse(w, levelId(currentLevel()), guid, p.x, p.z, stateOf(t));
  broadcast(w);
}

bool allowActivate(void* trigger, void* user)
{
  if (g_replaying || !g_session || !g_session->isConnected() || floor::ownsCurrentLevel() ||
      floor::isTownLevel(levelId(currentLevel())))
    return true;
  if (user != reinterpret_cast<void*>(localPlayer()))
    return true;
  auto* t = static_cast<engine::Unit*>(trigger);
  // The game retries every frame while the player stays on a trigger that doesn't change
  // locally; one request per trigger per moment is enough.
  static std::unordered_map<engine::Unit*, float> lastRequest;
  float now = clock();
  auto it = lastRequest.find(t);
  if (it != lastRequest.end() && now - it->second < 1.5f)
    return false;
  lastRequest[t] = now;
  Vec3 p = unitPosition(t);
  uint64_t guid = engine::field<uint64_t>(t, engine::kUnitGuid);
  int owner = floor::currentOwner();
  if (owner < 0)
    return false; // the floor is changing hands; the player can use it again in a moment
  TLMP_LOG("triggers: asking slot %d (runs this floor) to use %016llx at (%.1f, %.1f)", owner, guid, p.x, p.z);
  net::Writer w(Msg::TriggerRequest);
  w.u32(levelId(currentLevel()));
  w.u32(static_cast<uint32_t>(guid));
  w.u32(static_cast<uint32_t>(guid >> 32));
  w.f32(p.x);
  w.f32(p.z);
  floor::sendTo(static_cast<uint8_t>(owner), w, true);
  return false;
}

// Host: perform a client's trigger use as that player, then tell everyone the result.
static void hostApplyRequest(uint8_t fromSlot, uint32_t lid, uint64_t guid, float x, float z)
{
  engine::Level* level = currentLevel();
  if (lid != levelId(level))
    return;
  engine::Unit* t = findTrigger(level, guid, x, z);
  if (!t || !engine::field<uint8_t>(t, engine::kTriggerInteractable))
    return;
  int before = stateOf(t);
  party::setRemoteRequester(fromSlot);
  engine::TriggerActivate(t, avatarOf(fromSlot));
  party::setRemoteRequester(-1);
  if (stateOf(t) == before)
    return;
  Vec3 p = unitPosition(t);
  TLMP_LOG("triggers: slot %u used %016llx -> state %d", fromSlot, guid, stateOf(t));
  net::Writer w(Msg::TriggerUse);
  writeUse(w, lid, guid, p.x, p.z, stateOf(t));
  broadcast(w);
}

void tick()
{
  // Diagnostic: how many triggers each machine sees on a floor (a few seconds after arriving).
  static engine::Level* countedLevel = nullptr;
  static float countAt = 0.0f;
  if (engine::Level* lvl = currentLevel(); lvl != countedLevel)
  {
    countedLevel = lvl;
    countAt = clock() + 3.0f;
  }
  else if (countAt > 0.0f && clock() >= countAt && lvl)
  {
    countAt = 0.0f;
    int n = 0;
    forEachTrigger(lvl, [&](engine::Unit*) { n++; });
    TLMP_LOG("triggers: %d doors/levers/openables on level %08x", n, levelId(lvl));
  }

  if (!g_session || !g_session->isConnected() || !floor::ownsCurrentLevel())
    return;
  float now = clock();
  if (now < g_nextRoster)
    return;
  g_nextRoster = now + kRosterInterval;

  engine::Level* level = currentLevel();
  if (!level)
    return;
  struct Entry
  {
    uint64_t guid;
    Vec3 pos;
    int state;
  };
  std::vector<Entry> moved;
  forEachTrigger(level, [&](engine::Unit* t) {
    if (stateOf(t) != 0 && !engine::field<int>(t, engine::kTriggerLoops))
      moved.push_back({engine::field<uint64_t>(t, engine::kUnitGuid), unitPosition(t), stateOf(t)});
  });
  net::Writer w(Msg::TriggerRoster);
  w.u32(levelId(level));
  w.u16(static_cast<uint16_t>(moved.size()));
  for (const Entry& e : moved)
  {
    w.u32(static_cast<uint32_t>(e.guid));
    w.u32(static_cast<uint32_t>(e.guid >> 32));
    w.f32(e.pos.x);
    w.f32(e.pos.z);
    w.u32(static_cast<uint32_t>(e.state));
  }
  broadcast(w);
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (!g_session || !g_session->isConnected())
    return;
  uint32_t myLevel = levelId(currentLevel());

  if (type == Msg::TriggerRequest && floor::ownsCurrentLevel())
  {
    uint32_t lid = r.u32();
    uint64_t guid = r.u32();
    guid |= static_cast<uint64_t>(r.u32()) << 32;
    float x = r.f32(), z = r.f32();
    if (r.ok())
      hostApplyRequest(fromSlot, lid, guid, x, z);
    return;
  }
  if (type == Msg::TriggerUse)
  {
    uint32_t lid = r.u32();
    uint64_t guid = r.u32();
    guid |= static_cast<uint64_t>(r.u32()) << 32;
    float x = r.f32(), z = r.f32();
    int state = static_cast<int>(r.u32());
    if (!r.ok())
      return;
    if (lid == myLevel)
      applyState(guid, x, z, state);
    if (g_session->isHost())
    {
      // Relay to the other clients (the roster covers anyone on a different floor later).
      net::Writer w(Msg::TriggerUse);
      writeUse(w, lid, guid, x, z, state);
      for (uint8_t slot = 1; slot < net::kMaxPlayers; slot++)
        if (slot != fromSlot)
          g_session->sendTo(slot, w, true);
    }
  }
  else if (type == Msg::TriggerRoster)
  {
    uint32_t lid = r.u32();
    uint16_t count = r.u16();
    net::Writer relay(Msg::TriggerRoster); // host: pass a client owner's roster on
    relay.u32(lid);
    relay.u16(count);
    bool apply = lid == myLevel && !floor::ownsCurrentLevel();
    for (uint16_t i = 0; i < count && r.ok(); i++)
    {
      uint64_t guid = r.u32();
      guid |= static_cast<uint64_t>(r.u32()) << 32;
      float x = r.f32(), z = r.f32();
      int state = static_cast<int>(r.u32());
      if (!r.ok())
        break;
      relay.u32(static_cast<uint32_t>(guid));
      relay.u32(static_cast<uint32_t>(guid >> 32));
      relay.f32(x);
      relay.f32(z);
      relay.u32(static_cast<uint32_t>(state));
      if (apply)
        applyState(guid, x, z, state);
    }
    if (g_session->isHost() && r.ok())
      for (uint8_t slot = 1; slot < net::kMaxPlayers; slot++)
        if (slot != fromSlot)
          g_session->sendTo(slot, relay, true);
  }
}

// --- floor handover (floor.cpp): door states travel with the floor ----------------------------

void writeSnapshot(net::Writer& w)
{
  std::vector<std::pair<engine::Unit*, int>> moved;
  forEachTrigger(currentLevel(), [&](engine::Unit* t) {
    if (stateOf(t) != 0 && !engine::field<int>(t, engine::kTriggerLoops))
      moved.emplace_back(t, stateOf(t));
  });
  w.u16(static_cast<uint16_t>(moved.size()));
  for (auto& [t, state] : moved)
  {
    Vec3 p = unitPosition(t);
    uint64_t guid = engine::field<uint64_t>(t, engine::kUnitGuid);
    w.u32(static_cast<uint32_t>(guid));
    w.u32(static_cast<uint32_t>(guid >> 32));
    w.f32(p.x);
    w.f32(p.z);
    w.u32(static_cast<uint32_t>(state));
  }
}

void applySnapshot(net::Reader& r)
{
  uint16_t n = r.u16();
  for (uint16_t i = 0; i < n && r.ok(); i++)
  {
    uint64_t guid = r.u32();
    guid |= static_cast<uint64_t>(r.u32()) << 32;
    float x = r.f32(), z = r.f32();
    int state = static_cast<int>(r.u32());
    if (r.ok())
      applyState(guid, x, z, state);
  }
}

bool farthestTrigger(float* x, float* z)
{
  engine::Unit* me = reinterpret_cast<engine::Unit*>(localPlayer());
  if (!me)
    return false;
  Vec3 from = unitPosition(me);
  float best = -1.0f;
  for (engine::Unit* t : g_registry)
  {
    Vec3 p = unitPosition(t);
    float d = distXZ(p, from);
    if (d > best)
    {
      best = d;
      *x = p.x;
      *z = p.z;
    }
  }
  return best >= 0.0f;
}

} // namespace tlmp::game::triggers
