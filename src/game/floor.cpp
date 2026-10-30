#include "game/floor.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/internal.h"
#include "game/monsters.h"
#include "game/party.h"
#include "game/spawns.h"
#include "game/triggers.h"
#include "log.h"
#include "net/session.h"

#include <cwctype>
#include <map>
#include <vector>

namespace tlmp::game::floor {

using net::Msg;

constexpr uint8_t kToLevel = 0xFE;         // Relay destination: everyone on a level
constexpr float kDecideInterval = 0.25f;   // host: who runs the party floor
constexpr float kAnnounceInterval = 2.0f;
constexpr float kReleaseRetry = 3.0f;      // re-ask an owner that hasn't answered
constexpr float kTravelTimeout = 30.0f;
constexpr uint32_t kTravelPending = 0xFFFFFFFF;

static net::Session* g_session = nullptr;

// This machine.
static const void* g_loadingLevel = nullptr; // last level seen loading
static const void* g_ownedLevel = nullptr;   // the level this machine runs
static int g_expect = -1;                    // party travel: -1 nothing set, 0 copies only, 1 run it
static PartyFloor g_party;

// Host: who runs what.
struct Location
{
  uint32_t levelId = 0;
  std::wstring levelName, dungeon;
  int floor = 0;
};
static Location g_locations[net::kMaxPlayers]; // clients' reports (host's own is read live)
struct Snapshot
{
  std::vector<uint8_t> bytes;
  int author = -1; // slot whose floor save already holds this state
};
static std::map<uint32_t, Snapshot> g_snapshots; // by levelId
static struct
{
  uint32_t levelId = 0;
  int slot = -1;
  bool releasePending = false;
  float releaseAt = 0.0f;
  uint32_t travelFrom = 0; // while levelId == kTravelPending
  float travelAt = 0.0f;
} g_cur;
static Location g_lastSent; // client: last Location reported

static bool connected()
{
  return g_session && g_session->isConnected();
}

static uint8_t mySlot()
{
  return g_session ? g_session->localSlot() : 0;
}

uint32_t townLevelId()
{
  uint32_t h = 2166136261u; // as levelId(): FNV-1a of the upper-cased name, no seed for town
  for (const wchar_t* p = L"TOWN"; *p; p++)
    h = (h ^ static_cast<uint32_t>(*p)) * 16777619u;
  return h;
}

bool isTownLevel(uint32_t levelId)
{
  static const uint32_t town = townLevelId();
  return levelId == town;
}

int playersInGame()
{
  int n = 0;
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
    n += slotInGame(s) ? 1 : 0;
  return n < 1 ? 1 : n;
}

bool slotInGame(uint8_t slot)
{
  if (!connected())
    return false;
  if (slot == mySlot())
    return localPlayer() && currentLevel() && party::enteredFromContinue(); // not on the title screen
  const net::RemotePlayer& p = g_session->player(slot);
  return p.active && p.hasState && p.state.inGame;
}

uint32_t slotLevel(uint8_t slot)
{
  if (slot == mySlot())
    return levelId(currentLevel());
  return g_session->player(slot).state.levelId;
}

static Location localLocation()
{
  Location l;
  engine::Level* level = currentLevel();
  uintptr_t client = gameClient();
  if (!level || !client)
    return l;
  l.levelId = levelId(level);
  wchar_t buf[128] = L"";
  if (diag::readWStringAny(reinterpret_cast<uintptr_t>(level) + engine::kLevelName, buf, 128))
    l.levelName = buf;
  uint32_t dungeonObj = 0;
  buf[0] = 0;
  if (diag::readU32(client + engine::kClientDungeon, dungeonObj) && dungeonObj &&
      diag::readWStringAny(dungeonObj + engine::kDungeonName, buf, 128))
    l.dungeon = buf;
  l.floor = engine::field<int>(level, engine::kLevelFloor);
  return l;
}

// --- this machine's role -------------------------------------------------------------------

bool ownsLevel(const void* level)
{
  if (!connected())
    return true;
  return level && level == g_ownedLevel;
}

bool ownsCurrentLevel()
{
  return ownsLevel(currentLevel());
}

int currentOwner()
{
  if (!connected())
    return -1;
  if (ownsCurrentLevel())
    return mySlot();
  if (g_party.known && g_party.levelId == levelId(currentLevel()))
    return g_party.owner;
  return -1;
}

const PartyFloor& partyFloor()
{
  return g_party;
}

bool partyFloorElsewhere()
{
  if (!connected() || !g_party.known)
    return false;
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
    if (s != mySlot() && slotInGame(s) && slotLevel(s) == g_party.levelId)
      return true;
  return false;
}

void onLevelLoading(void* level)
{
  if (!level || level == g_loadingLevel)
    return;
  g_loadingLevel = level;
  bool own;
  const char* why;
  if (g_expect >= 0)
  {
    own = g_expect == 1;
    why = "party travel";
  }
  else
  {
    own = !partyFloorElsewhere();
    why = own ? "nobody else is on a floor" : "joining the party's floor";
  }
  g_expect = -1;
  g_ownedLevel = own ? level : nullptr;
  if (connected())
    TLMP_LOG("floor: loading level %p: %s (%s)", level, own ? "I run it" : "copies only", why);
}

void expectOwnership(bool own)
{
  g_expect = own ? 1 : 0;
}

static void writeSnapshot(net::Writer& w)
{
  monsters::writeSnapshot(w);
  triggers::writeSnapshot(w);
  spawns::writeSnapshot(w);
}

// Turns this machine into the owner of its current floor, applying a snapshot if given.
static void becomeOwner(const std::vector<uint8_t>* snapshot)
{
  engine::Level* level = currentLevel();
  if (!level)
    return;
  g_ownedLevel = level;
  if (snapshot && !snapshot->empty())
  {
    net::Reader r(snapshot->data(), snapshot->size());
    monsters::becomeOwner(&r);
    triggers::applySnapshot(r);
    spawns::applySnapshot(r);
  }
  else
  {
    monsters::becomeOwner(nullptr);
  }
  TLMP_LOG("floor: I run level %08x now%s", levelId(level), snapshot && !snapshot->empty() ? " (from a snapshot)" : "");
}

// Stops running the current floor: returns its snapshot; monsters stay as copies.
static std::vector<uint8_t> giveUp()
{
  net::Writer w(Msg::FloorSnapshot); // the type byte is skipped when read back
  writeSnapshot(w);
  std::vector<uint8_t> bytes(w.data() + 1, w.data() + w.size());
  monsters::becomeMirror();
  g_ownedLevel = nullptr;
  TLMP_LOG("floor: handed level %08x over (%u bytes)", levelId(currentLevel()), static_cast<unsigned>(bytes.size()));
  return bytes;
}

void onLevelDestroyed(void* level)
{
  if (level == g_loadingLevel)
    g_loadingLevel = nullptr;
  if (!connected() || level != g_ownedLevel)
    return;
  // Leaving the floor we run: what it looked like goes to the host for whoever comes next.
  uint32_t lid = levelId(static_cast<engine::Level*>(level));
  net::Writer w(Msg::FloorSnapshot);
  w.u32(lid);
  net::Writer body(Msg::FloorSnapshot);
  writeSnapshot(body);
  std::vector<uint8_t> bytes(body.data() + 1, body.data() + body.size());
  g_ownedLevel = nullptr;
  if (lid == 0 || isTownLevel(lid))
    return;
  if (g_session->isHost())
  {
    g_snapshots[lid] = Snapshot{bytes, mySlot()};
    if (g_cur.levelId == lid)
      g_cur.slot = -1;
  }
  else
  {
    w.bytes(bytes);
    g_session->sendTo(0, w, true);
  }
  TLMP_LOG("floor: left level %08x that I ran; snapshot kept by the host (%u bytes)", lid,
           static_cast<unsigned>(bytes.size()));
}

// --- routing ------------------------------------------------------------------------------

void sendTo(uint8_t slot, const net::Writer& w, bool reliable)
{
  if (!connected() || slot == mySlot() || slot >= net::kMaxPlayers)
    return;
  if (g_session->isHost() || slot == 0)
  {
    g_session->sendTo(slot, w, reliable);
    return;
  }
  net::Writer relay(Msg::Relay);
  relay.u8(slot);
  relay.u8(mySlot());
  relay.u32(0);
  relay.u8(reliable ? 1 : 0);
  relay.message(w);
  g_session->sendTo(0, relay, reliable);
}

void sendToFloor(const net::Writer& w, bool reliable)
{
  if (!connected())
    return;
  uint32_t lid = levelId(currentLevel());
  if (g_session->isHost())
  {
    for (uint8_t s = 1; s < net::kMaxPlayers; s++)
      if (slotInGame(s) && slotLevel(s) == lid)
        g_session->sendTo(s, w, reliable);
    return;
  }
  net::Writer relay(Msg::Relay);
  relay.u8(kToLevel);
  relay.u8(mySlot());
  relay.u32(lid);
  relay.u8(reliable ? 1 : 0);
  relay.message(w);
  g_session->sendTo(0, relay, reliable);
}

void onRelay(uint8_t fromSlot, net::Reader& r, DispatchFn dispatch)
{
  uint8_t dest = r.u8();
  uint8_t from = r.u8();
  uint32_t lid = r.u32();
  bool reliable = r.u8() != 0;
  std::vector<uint8_t> msg = r.bytes();
  if (!r.ok() || msg.empty())
    return;
  if (g_session->isHost())
  {
    from = fromSlot; // whoever really sent it
    auto forward = [&](uint8_t to) {
      net::Writer relay(Msg::Relay);
      relay.u8(dest);
      relay.u8(from);
      relay.u32(lid);
      relay.u8(reliable ? 1 : 0);
      relay.bytes(msg);
      g_session->sendTo(to, relay, reliable);
    };
    if (dest == kToLevel)
    {
      for (uint8_t s = 1; s < net::kMaxPlayers; s++)
        if (s != from && slotInGame(s) && slotLevel(s) == lid)
          forward(s);
      if (levelId(currentLevel()) != lid)
        return; // the host isn't there itself
    }
    else if (dest != 0)
    {
      if (dest < net::kMaxPlayers)
        forward(dest);
      return;
    }
  }
  net::Reader inner(msg.data(), msg.size());
  auto type = static_cast<Msg>(inner.u8());
  if (inner.ok())
    dispatch(from, type, inner);
}

// --- host: deciding who runs the party floor ------------------------------------------------

static void announce()
{
  net::Writer w(Msg::PartyFloor);
  w.u32(g_party.known ? g_party.levelId : 0);
  w.str(toUtf8(g_party.levelName.c_str()));
  w.str(toUtf8(g_party.dungeon.c_str()));
  w.u32(static_cast<uint32_t>(g_party.floor));
  w.u8(static_cast<uint8_t>(g_party.owner < 0 ? 0xFF : g_party.owner));
  g_session->sendToAll(w, true);
}

static void grant(int slot, uint32_t lid)
{
  auto it = g_snapshots.find(lid);
  // A floor save written by the grantee itself already holds the snapshot's state.
  const std::vector<uint8_t>* snapshot =
    it != g_snapshots.end() && it->second.author != slot ? &it->second.bytes : nullptr;
  TLMP_LOG("floor: [host] slot %d runs level %08x now%s", slot, lid, snapshot ? " (with snapshot)" : "");
  if (slot == 0)
  {
    if (levelId(currentLevel()) == lid)
      becomeOwner(snapshot);
  }
  else
  {
    net::Writer w(Msg::OwnerGrant);
    w.u32(lid);
    w.bytes(snapshot ? *snapshot : std::vector<uint8_t>{});
    g_session->sendTo(static_cast<uint8_t>(slot), w, true);
  }
  g_cur.levelId = lid;
  g_cur.slot = slot;
  g_cur.releasePending = false;
}

static void release(int slot, uint32_t lid)
{
  g_cur.releasePending = true;
  g_cur.releaseAt = clock();
  if (slot == 0)
  {
    g_snapshots[lid] = Snapshot{giveUp(), 0};
    g_cur.slot = -1;
    g_cur.releasePending = false;
    return;
  }
  TLMP_LOG("floor: [host] asking slot %d to hand level %08x over", slot, lid);
  net::Writer w(Msg::OwnerRelease);
  w.u32(lid);
  g_session->sendTo(static_cast<uint8_t>(slot), w, true);
}

static void hostDecide()
{
  // The party's floor: the host's dungeon floor if it is on one, else the lowest slot's.
  uint32_t lid = 0;
  for (uint8_t s = 0; s < net::kMaxPlayers && !lid; s++)
    if (slotInGame(s) && slotLevel(s) && !isTownLevel(slotLevel(s)))
      lid = slotLevel(s);
  int desired = -1;
  for (uint8_t s = 0; s < net::kMaxPlayers && desired < 0; s++)
    if (lid && slotInGame(s) && slotLevel(s) == lid)
      desired = s;

  // Party floor details for joining and returning from town.
  PartyFloor pf;
  if (lid)
  {
    pf.known = true;
    pf.levelId = lid;
    Location l = slotLevel(0) == lid && slotInGame(0) ? localLocation() : Location{};
    for (uint8_t s = 1; s < net::kMaxPlayers && l.levelId != lid; s++)
      if (g_locations[s].levelId == lid)
        l = g_locations[s];
    pf.levelName = l.levelName;
    pf.dungeon = l.dungeon;
    pf.floor = l.floor;
  }

  // A party trip chose the owner of the floor it went to; take that over once they're there.
  if (g_cur.levelId == kTravelPending)
  {
    if (lid && lid != g_cur.travelFrom && g_cur.slot >= 0 && slotLevel(static_cast<uint8_t>(g_cur.slot)) == lid)
    {
      g_cur.levelId = lid;
      auto it = g_snapshots.find(lid);
      if (it != g_snapshots.end() && it->second.author != g_cur.slot)
        grant(g_cur.slot, lid); // been here before and someone else ran it last: their state wins
    }
    else if (clock() - g_cur.travelAt > kTravelTimeout)
    {
      g_cur.levelId = 0;
      g_cur.slot = -1;
    }
  }

  if (g_cur.levelId != kTravelPending)
  {
    bool ownerThere = g_cur.slot >= 0 && g_cur.levelId == lid && slotInGame(static_cast<uint8_t>(g_cur.slot)) &&
                      slotLevel(static_cast<uint8_t>(g_cur.slot)) == lid;
    if (!lid)
    {
      g_cur.levelId = 0;
      g_cur.slot = -1;
      g_cur.releasePending = false;
    }
    else if (!ownerThere)
    {
      grant(desired, lid); // nobody runs it (left, disconnected, or a floor nobody was on)
    }
    else if (g_cur.slot != desired)
    {
      if (!g_cur.releasePending || clock() - g_cur.releaseAt > kReleaseRetry)
        release(g_cur.slot, lid);
      if (g_cur.slot < 0)
        grant(desired, lid); // the host gave it up on the spot
    }
  }
  pf.owner = g_cur.levelId == lid ? g_cur.slot : -1;

  static float nextAnnounce = 0.0f;
  bool changed = pf.known != g_party.known || pf.levelId != g_party.levelId || pf.owner != g_party.owner ||
                 pf.floor != g_party.floor || pf.dungeon != g_party.dungeon;
  g_party = pf;
  if (changed || clock() >= nextAnnounce)
  {
    nextAnnounce = clock() + kAnnounceInterval;
    announce();
    if (changed)
      TLMP_LOG("floor: [host] party floor %08x (%ls floor %d), run by slot %d", pf.levelId, pf.dungeon.c_str(), pf.floor,
               pf.owner);
  }
}

// Called by party travel (host) with the slot that will run the destination.
void onPartyTravel(int ownerSlot)
{
  g_cur.travelFrom = g_party.levelId;
  g_cur.levelId = kTravelPending;
  g_cur.slot = ownerSlot;
  g_cur.travelAt = clock();
  g_cur.releasePending = false;
}

// --- public ------------------------------------------------------------------------------------

void attach(net::Session* session)
{
  g_session = session;
}

void tick()
{
  if (engine::Level* level = currentLevel(); level != g_loadingLevel && level)
    onLevelLoading(level); // a level that added no units while loading
  if (!connected())
    return;
  if (g_session->isHost())
  {
    static float next = 0.0f;
    if (clock() >= next)
    {
      next = clock() + kDecideInterval;
      hostDecide();
    }
    return;
  }
  // Clients report where they are, for the host's party floor details.
  Location l = localLocation();
  if (l.levelId != g_lastSent.levelId || l.floor != g_lastSent.floor || l.dungeon != g_lastSent.dungeon)
  {
    g_lastSent = l;
    net::Writer w(Msg::Location);
    w.u32(l.levelId);
    w.str(toUtf8(l.levelName.c_str()));
    w.str(toUtf8(l.dungeon.c_str()));
    w.u32(static_cast<uint32_t>(l.floor));
    g_session->sendTo(0, w, true);
  }
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (!connected())
    return;
  bool host = g_session->isHost();
  if (host && type == Msg::Location && fromSlot < net::kMaxPlayers)
  {
    Location l;
    l.levelId = r.u32();
    l.levelName = fromUtf8(r.str());
    l.dungeon = fromUtf8(r.str());
    l.floor = static_cast<int>(r.u32());
    if (r.ok())
      g_locations[fromSlot] = l;
  }
  else if (host && type == Msg::FloorSnapshot)
  {
    uint32_t lid = r.u32();
    std::vector<uint8_t> bytes = r.bytes();
    if (!r.ok())
      return;
    if (!bytes.empty())
      g_snapshots[lid] = Snapshot{std::move(bytes), fromSlot};
    TLMP_LOG("floor: [host] slot %u sent level %08x's snapshot", fromSlot, lid);
    if (g_cur.levelId == lid && g_cur.slot == fromSlot)
    {
      g_cur.slot = -1; // released (or left): hostDecide grants it to whoever should run it
      g_cur.releasePending = false;
    }
  }
  else if (!host && type == Msg::PartyFloor)
  {
    PartyFloor pf;
    uint32_t lid = r.u32();
    pf.levelName = fromUtf8(r.str());
    pf.dungeon = fromUtf8(r.str());
    pf.floor = static_cast<int>(r.u32());
    uint8_t owner = r.u8();
    if (!r.ok())
      return;
    pf.known = lid != 0;
    pf.levelId = lid;
    pf.owner = owner == 0xFF ? -1 : owner;
    if (pf.owner != g_party.owner || pf.levelId != g_party.levelId)
      TLMP_LOG("floor: party floor %08x (%ls floor %d), run by slot %d", pf.levelId, pf.dungeon.c_str(), pf.floor,
               pf.owner);
    g_party = pf;
    // Two machines loaded this floor at once and each started running it: the host's choice stands.
    if (pf.known && pf.owner >= 0 && pf.owner != mySlot() && pf.levelId == levelId(currentLevel()) &&
        ownsCurrentLevel())
    {
      TLMP_LOG("floor: slot %d runs this floor, not me", pf.owner);
      monsters::discardOwn();
      g_ownedLevel = nullptr;
    }
  }
  else if (!host && type == Msg::OwnerGrant)
  {
    uint32_t lid = r.u32();
    std::vector<uint8_t> bytes = r.bytes();
    if (!r.ok())
      return;
    if (lid != levelId(currentLevel()))
    {
      TLMP_LOG("floor: granted level %08x but I'm not on it", lid);
      return;
    }
    becomeOwner(&bytes);
  }
  else if (!host && type == Msg::OwnerRelease)
  {
    uint32_t lid = r.u32();
    if (!r.ok())
      return;
    net::Writer w(Msg::FloorSnapshot);
    w.u32(lid);
    if (lid == levelId(currentLevel()) && ownsCurrentLevel())
      w.bytes(giveUp());
    else
      w.bytes({}); // not ours (any more): nothing to hand over
    g_session->sendTo(0, w, true);
  }
}

void onPlayerLeft(uint8_t slot)
{
  if (slot < net::kMaxPlayers)
    g_locations[slot] = Location{};
  if (g_cur.slot == slot)
  {
    g_cur.slot = -1; // its copies on the others' machines are the best state left
    g_cur.releasePending = false;
  }
}

void onReconnected()
{
  g_lastSent = Location{};
}

void endSession()
{
  g_snapshots.clear();
  g_cur = {};
  g_party = PartyFloor{};
  g_ownedLevel = nullptr;
  g_expect = -1;
}

} // namespace tlmp::game::floor
