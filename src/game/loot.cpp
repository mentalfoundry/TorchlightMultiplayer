#include "game/loot.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/internal.h"
#include "game/savestate.h"
#include "log.h"
#include "net/session.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace tlmp::game::loot {

using net::Msg;

constexpr float kClaimInterval = 1.0f; // re-ask the host at most this often per item
constexpr unsigned kDetailedLogs = 30;

static net::Session* g_session = nullptr;
static bool g_spawning = false; // our own AddItem calls
static bool g_granted = false;  // our own PickUp call after the host said yes
static uint32_t g_nextSeq = 1;
static uint8_t g_mySlot = net::kInvalidSlot; // last slot we had, for cleaning up after a disconnect
static unsigned g_addLogs = 0;

struct Shared
{
  uint32_t levelId = 0;
  Vec3 pos{};
  uint64_t guid = 0;
  std::vector<uint8_t> blob;   // the item's save state, as written by the game
  void* local = nullptr;       // this machine's unit for it, while on the ground here
  engine::Level* localLevel = nullptr;
  bool spawnFailed = false;
  float lastClaim = -10.0f;
};
static std::map<uint32_t, Shared> g_items; // by key: dropper's slot << 24 | sequence

static bool connected()
{
  return g_session && g_session->isConnected();
}

static uint8_t dropperOf(uint32_t key)
{
  return static_cast<uint8_t>(key >> 24);
}

// --- the game's item save state ------------------------------------------------------------

static bool serialize(void* item, std::vector<uint8_t>& out)
{
  void* state = savestate::create(engine::kItemStateSize, engine::ItemStateCtor);
  if (!state)
    return false;
  engine::virtualFn<void(__thiscall*)(void*, void*, int, char)>(item, engine::kItemCaptureSlot)(item, state, -1, 0);
  bool ok = savestate::toBytes(state, engine::ItemStateWrite, out);
  savestate::destroy(state);
  return ok;
}

// Builds the item from its saved state and puts it on the ground here.
static void* spawn(const Shared& s, engine::Level* level)
{
  uintptr_t client = gameClient();
  void* resources = client ? engine::field<void*>(reinterpret_cast<void*>(client), engine::kClientResources) : nullptr;
  void* state = resources ? savestate::create(engine::kItemStateSize, engine::ItemStateCtor) : nullptr;
  if (!state)
    return nullptr;
  if (!savestate::fromBytes(state, engine::ItemStateRead, *reinterpret_cast<const uint32_t*>(engine::kItemStateVersion),
                            s.blob))
  {
    savestate::destroy(state);
    return nullptr;
  }
  uint64_t guid = engine::field<uint64_t>(state, engine::kItemStateGuid);
  void* item = nullptr;
  if (guid == s.guid)
    item = engine::CreateByGuid(resources, static_cast<uint32_t>(guid), static_cast<uint32_t>(guid >> 32), 1, 1, 0);
  const char* cls = item ? diag::rttiName(reinterpret_cast<uintptr_t>(item)) : nullptr;
  if (!item || !cls || (!std::strstr(cls, "CItem") && !std::strstr(cls, "CEquipment")))
  {
    TLMP_LOG("loot: could not rebuild item %016llx (state GUID %016llx, class %s)", s.guid, guid, cls ? cls : "-");
    if (item)
      engine::destroyUnit(item);
    savestate::destroy(state);
    return nullptr;
  }
  // Same order as a floor load (0x41413F): onto the ground, then the saved state.
  g_spawning = true;
  engine::AddItemToLevel(level, item, &s.pos, 0);
  g_spawning = false;
  engine::virtualFn<void(__thiscall*)(void*, void*)>(item, engine::kItemRestoreSlot)(item, state);
  savestate::destroy(state);
  return item;
}

static void removeLocal(Shared& s, const char* why)
{
  if (!s.local)
    return;
  if (s.localLevel)
  {
    engine::RemoveItemFromLevel(s.localLevel, s.local, 1);
    engine::destroyUnit(s.local);
  }
  TLMP_LOG("loot: removed my copy %p (%s)", s.local, why);
  s.local = nullptr;
  s.localLevel = nullptr;
}

static Shared* findByUnit(const void* item, uint32_t* key = nullptr)
{
  for (auto& [k, s] : g_items)
    if (s.local == item)
    {
      if (key)
        *key = k;
      return &s;
    }
  return nullptr;
}

static void writeItem(net::Writer& w, uint32_t key, const Shared& s)
{
  w.u32(key);
  w.u32(s.levelId);
  w.f32(s.pos.x);
  w.f32(s.pos.y);
  w.f32(s.pos.z);
  w.u32(static_cast<uint32_t>(s.guid));
  w.u32(static_cast<uint32_t>(s.guid >> 32));
  w.bytes(s.blob);
}

static bool readItem(net::Reader& r, uint32_t& key, Shared& s)
{
  key = r.u32();
  s.levelId = r.u32();
  s.pos.x = r.f32();
  s.pos.y = r.f32();
  s.pos.z = r.f32();
  s.guid = r.u32();
  s.guid |= static_cast<uint64_t>(r.u32()) << 32;
  s.blob = r.bytes();
  return r.ok();
}

// --- host decides pickups ----------------------------------------------------------------------

// Host: `slot` gets the item; everyone else drops their copy.
static void hostGive(uint32_t key, uint8_t slot)
{
  auto it = g_items.find(key);
  if (it == g_items.end())
    return;
  net::Writer w(Msg::ItemTaken);
  w.u32(key);
  w.u8(slot);
  g_session->sendToAll(w, true);
  TLMP_LOG("loot: [host] item %08x goes to slot %u", key, slot);
  if (slot != 0)
    removeLocal(it->second, "someone else picked it up");
  g_items.erase(it);
}

// --- public ------------------------------------------------------------------------------------

void attach(net::Session* session)
{
  g_session = session;
}

bool beforeAddItem(void* level, void* item, uintptr_t caller)
{
  if (!connected() || g_spawning || !item)
    return false;
  uintptr_t client = gameClient();
  void* ui = client ? engine::field<void*>(reinterpret_cast<void*>(client), engine::kClientGameUI) : nullptr;
  void* held = ui ? engine::field<void*>(ui, engine::kGameUIHeldItem) : nullptr;
  bool drop = held == item;
  if (g_addLogs < kDetailedLogs)
  {
    g_addLogs++;
    const char* cls = diag::rttiName(reinterpret_cast<uintptr_t>(item));
    TLMP_LOG("loot: item %p (%s, %016llx) put on the ground of %p from %08x; held item %p -> %s", item,
             cls ? cls : "?", engine::field<uint64_t>(item, engine::kUnitGuid), level, static_cast<unsigned>(caller),
             held, drop ? "player drop (shared)" : "not shared");
  }
  return drop;
}

void afterAddItem(void* level, void* item, bool playerDrop)
{
  if (!playerDrop || !connected())
    return;
  Shared s;
  if (!serialize(item, s.blob))
  {
    TLMP_LOG("loot: could not save dropped item %p; not shared", item);
    return;
  }
  s.levelId = levelId(static_cast<engine::Level*>(level));
  s.pos = unitPosition(item);
  s.guid = engine::field<uint64_t>(item, engine::kUnitGuid);
  s.local = item;
  s.localLevel = static_cast<engine::Level*>(level);
  uint32_t key = (static_cast<uint32_t>(g_session->localSlot()) << 24) | (g_nextSeq++ & 0xFFFFFF);

  net::Writer w(g_session->isHost() ? Msg::ItemSpawn : Msg::ItemDrop);
  writeItem(w, key, s);
  if (g_session->isHost())
    g_session->sendToAll(w, true);
  else
    g_session->sendTo(0, w, true);
  TLMP_LOG("loot: I dropped %016llx at (%.1f, %.1f): shared as %08x (%u bytes)", s.guid, s.pos.x, s.pos.z, key,
           static_cast<unsigned>(s.blob.size()));
  g_items[key] = std::move(s);
}

bool allowPickUp(void* character, void* item, void* level)
{
  if (!connected() || g_granted)
    return true;
  uint32_t key = 0;
  Shared* s = findByUnit(item, &key);
  if (!s)
    return true; // an ordinary item: this player's own loot
  if (reinterpret_cast<uintptr_t>(character) != localPlayer())
    return false; // pets and copies never take shared items

  if (g_session->isHost())
  {
    // First come, first served: nobody has claimed it before us, or it would be gone.
    hostGive(key, 0);
    return true;
  }
  float now = clock();
  if (now - s->lastClaim >= kClaimInterval)
  {
    s->lastClaim = now;
    net::Writer w(Msg::ItemClaim);
    w.u32(key);
    g_session->sendTo(0, w, true);
    TLMP_LOG("loot: asking the host for item %08x", key);
  }
  (void)level;
  return false;
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (!connected())
    return;
  bool host = g_session->isHost();

  if (type == Msg::ItemDrop && host)
  {
    uint32_t key;
    Shared s;
    if (!readItem(r, key, s) || dropperOf(key) != fromSlot)
      return;
    TLMP_LOG("loot: [host] slot %u dropped %016llx (%08x)", fromSlot, s.guid, key);
    net::Writer w(Msg::ItemSpawn);
    writeItem(w, key, s);
    for (uint8_t slot = 1; slot < net::kMaxPlayers; slot++)
      if (slot != fromSlot && g_session->player(slot).active)
        g_session->sendTo(slot, w, true);
    g_items[key] = std::move(s); // spawned here by tick() while we are on that floor
  }
  else if (type == Msg::ItemSpawn && !host)
  {
    uint32_t key;
    Shared s;
    if (!readItem(r, key, s))
      return;
    TLMP_LOG("loot: slot %u dropped %016llx (%08x)", dropperOf(key), s.guid, key);
    g_items[key] = std::move(s);
  }
  else if (type == Msg::ItemClaim && host)
  {
    uint32_t key = r.u32();
    if (r.ok() && g_items.count(key))
      hostGive(key, fromSlot);
  }
  else if (type == Msg::ItemTaken && !host)
  {
    uint32_t key = r.u32();
    uint8_t slot = r.u8();
    auto it = g_items.find(key);
    if (!r.ok() || it == g_items.end())
      return;
    Shared& s = it->second;
    if (slot == g_session->localSlot() && s.local && s.localLevel)
    {
      TLMP_LOG("loot: the host gave me item %08x", key);
      g_granted = true;
      engine::PickUp(reinterpret_cast<engine::Unit*>(localPlayer()), s.local, s.localLevel);
      g_granted = false;
    }
    else
      removeLocal(s, "someone else picked it up");
    g_items.erase(it);
  }
}

static void dropCopies(uint8_t me);

void tick()
{
  if (!connected())
  {
    if (!g_items.empty())
      dropCopies(g_mySlot); // the session ended under us
    return;
  }
  g_mySlot = g_session->localSlot();
  engine::Level* level = currentLevel();
  if (!level || !localPlayer())
    return;
  uint32_t lid = levelId(level);
  for (auto& [key, s] : g_items)
  {
    if (s.local || s.spawnFailed || s.levelId != lid)
      continue;
    s.local = spawn(s, level);
    s.localLevel = s.local ? level : nullptr;
    s.spawnFailed = !s.local;
    if (s.local)
      TLMP_LOG("loot: put shared item %08x (%016llx) on the ground at (%.1f, %.1f)", key, s.guid, s.pos.x, s.pos.z);
  }
}

void beforeSave()
{
  unsigned removed = 0;
  for (auto& [key, s] : g_items)
  {
    if (s.local)
    {
      removeLocal(s, "the game is saving");
      removed++;
    }
  }
  if (removed)
    TLMP_LOG("loot: took %u shared items off the ground before the game saves", removed);
}

void onLevelDestroyed(void* level)
{
  for (auto& [key, s] : g_items)
    if (s.localLevel == level)
    {
      s.local = nullptr;
      s.localLevel = nullptr;
    }
}

// Items this player dropped stay where they are as ordinary items; everyone else's copies go.
static void dropCopies(uint8_t me)
{
  for (auto& [key, s] : g_items)
    if (dropperOf(key) != me)
      removeLocal(s, "the session ended");
  g_items.clear();
}

void endSession()
{
  dropCopies(connected() ? g_session->localSlot() : g_mySlot);
}

void forEachShared(void (*fn)(uint32_t key, void* unit, void* ctx), void* ctx)
{
  for (auto& [key, s] : g_items)
    fn(key, s.local, ctx);
}

} // namespace tlmp::game::loot
