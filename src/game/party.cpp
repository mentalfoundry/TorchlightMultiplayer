#include "game/party.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/floor.h"
#include "game/game.h"
#include "game/internal.h"
#include "game/triggers.h"
#include "log.h"
#include "net/session.h"

#include <cstdio>
#include <cwchar>
#include <string>

namespace tlmp::game::party {

using net::Msg;

constexpr float kGatherRadius = 8.0f;     // everyone within this of the exit
constexpr float kCancelRadius = 12.0f;    // requester walked away from the exit
constexpr float kNoticeInterval = 2.5f;
constexpr int kCountdownSeconds = 3;      // once everyone is at the exit
constexpr uint8_t kEveryone = 0xFF;       // TravelGo: everyone on the level

static net::Session* g_session = nullptr;
static bool g_executing = false; // running an agreed move: let ChangeLevel through
static int g_enteringGame = 0;   // inside the game's enter-game routine
static int g_remoteRequester = -1;

static bool g_enteredFromContinue = false;
static bool g_joinPending = false;
static float g_joinAt = 0.0f;

struct Pending
{
  bool active = false;
  uint8_t requester = 0;
  TravelArgs args;
  Vec3 spot{};
  uint32_t level = 0;
  int lastGathered = -1;
  std::string lastReason;
  float nextNotice = 0.0f;
  float countdownStart = -1.0f; // < 0: still waiting for the party
  int lastCountdown = -1;
};
static Pending g_pending; // host only

static void writeArgs(net::Writer& w, const TravelArgs& a)
{
  w.str(toUtf8(a.dungeon.c_str()));
  w.u32(static_cast<uint32_t>(a.mode));
  w.u32(static_cast<uint32_t>(a.floor));
  w.u8(static_cast<uint8_t>(a.waypoint));
  w.str(toUtf8(a.warpName.c_str()));
  w.u8(static_cast<uint8_t>(a.flag));
}

static TravelArgs readArgs(net::Reader& r)
{
  TravelArgs a;
  a.dungeon = fromUtf8(r.str());
  a.mode = static_cast<int>(r.u32());
  a.floor = static_cast<int>(r.u32());
  a.waypoint = static_cast<char>(r.u8());
  a.warpName = fromUtf8(r.str());
  a.flag = static_cast<char>(r.u8());
  return a;
}

static bool connected()
{
  return g_session && g_session->isConnected();
}

static Vec3 localPosition()
{
  uintptr_t p = localPlayer();
  return p ? unitPosition(reinterpret_cast<engine::Unit*>(p)) : Vec3{};
}

// Town portals and resurrect-in-town ask for L"Town" / mode -99 (0x4E1513, 0x4190EF).
static bool goesToTown(const TravelArgs& a)
{
  return a.mode == -99 || _wcsicmp(a.dungeon.c_str(), L"Town") == 0;
}

static void execute(const TravelArgs& a, const char* notice)
{
  void* client = reinterpret_cast<void*>(gameClient());
  if (!client)
    return;
  TLMP_LOG("party: travelling to '%ls' (mode %d, floor %d, warp '%ls')", a.dungeon.c_str(), a.mode, a.floor,
           a.warpName.c_str());
  if (notice)
    showNotice(notice, 3.0f);
  auto changeLevel = reinterpret_cast<engine::ChangeLevelFn>(engine::kChangeLevel);
  g_executing = true;
  changeLevel(client, vs::make(a.dungeon.c_str()), a.mode, a.floor, a.waypoint, vs::make(a.warpName.c_str()),
              a.flag);
  g_executing = false;
}

// To the party's floor, as console DESCEND/ASCEND do; whoever runs it stays in charge.
static bool joinPartyFloor(const char* notice)
{
  const floor::PartyFloor& pf = floor::partyFloor();
  if (!pf.known || pf.dungeon.empty())
    return false;
  TravelArgs a;
  a.dungeon = pf.dungeon;
  a.mode = 1;
  a.floor = pf.floor;
  a.flag = 1;
  TLMP_LOG("party: going to the party's floor '%ls' (%ls floor %d)", pf.levelName.c_str(), pf.dungeon.c_str(), pf.floor);
  floor::expectOwnership(false);
  execute(a, notice);
  return true;
}

static std::string nameOf(uint8_t slot)
{
  if (slot == g_session->localSlot())
  {
    std::string n = localPlayerInfo().charName;
    return n.empty() ? "You" : n;
  }
  const net::RemotePlayer& p = g_session->player(slot);
  return displayName(slot, p.info.charName.empty() ? p.name : p.info.charName);
}

static bool slotDead(uint8_t slot, bool* hardcore)
{
  if (slot == g_session->localSlot())
  {
    uintptr_t me = localPlayer();
    if (!me)
      return false;
    *hardcore = engine::field<uint8_t>(reinterpret_cast<void*>(me), engine::kPlayerHardcore) != 0;
    int ai = engine::field<int>(reinterpret_cast<void*>(me), engine::kUnitAIState);
    return ai == engine::kAIStateDying || ai == engine::kAIStateDead;
  }
  const net::RemotePlayer& p = g_session->player(slot);
  *hardcore = p.state.hardcore;
  return p.state.dead;
}

static Vec3 slotPosition(uint8_t slot)
{
  if (slot == g_session->localSlot())
    return localPosition();
  const net::PlayerState& s = g_session->player(slot).state;
  return Vec3{s.pos[0], s.pos[1], s.pos[2]};
}

static void sendStatus(bool waiting, int gathered, int total, int countdown, const std::string& reason)
{
  net::Writer w(Msg::TravelStatus);
  w.u8(waiting ? 1 : 0);
  w.u8(static_cast<uint8_t>(gathered));
  w.u8(static_cast<uint8_t>(total));
  w.u8(static_cast<uint8_t>(countdown));
  w.str(reason);
  g_session->sendToAll(w, true);
}

static void noticeCountdown(int seconds)
{
  char msg[64];
  std::snprintf(msg, sizeof(msg), "Everyone is here. Leaving in %d...", seconds);
  showNotice(msg, 1.2f);
}

static void noticeWaiting(int gathered, int total, const std::string& reason)
{
  char msg[192];
  if (reason.empty())
    std::snprintf(msg, sizeof(msg), "Waiting for the party at the exit (%d/%d)", gathered, total);
  else
    std::snprintf(msg, sizeof(msg), "%s (%d/%d at the exit)", reason.c_str(), gathered, total);
  showNotice(msg, kNoticeInterval + 0.5f);
}

static void cancelPending(const char* why)
{
  if (!g_pending.active)
    return;
  TLMP_LOG("party: travel cancelled (%s)", why);
  g_pending.active = false;
  sendStatus(false, 0, 0, 0, "");
  showNotice("Party travel cancelled", 3.0f);
}

static void hostRequest(uint8_t requester, const TravelArgs& a, const Vec3& spot)
{
  if (g_pending.active && g_pending.requester != requester)
    return; // someone else's trip is already being organised
  g_pending = Pending{};
  g_pending.active = true;
  g_pending.requester = requester;
  g_pending.args = a;
  g_pending.spot = spot;
  g_pending.level = floor::slotLevel(requester);
  TLMP_LOG("party: slot %u wants to travel to '%ls' (mode %d); waiting for everyone at (%.1f, %.1f)", requester,
           a.dungeon.c_str(), a.mode, spot.x, spot.z);
}

static void hostGo()
{
  TravelArgs a = g_pending.args;
  uint32_t from = g_pending.level;
  g_pending.active = false;
  // Whoever will run the floor they arrive on: the host if it goes, else the lowest slot going.
  int owner = -1;
  for (uint8_t s = 0; s < net::kMaxPlayers && owner < 0; s++)
    if (floor::slotInGame(s) && floor::slotLevel(s) == from)
      owner = s;
  floor::onPartyTravel(owner);
  net::Writer w(Msg::TravelGo);
  w.u32(from);
  w.u8(static_cast<uint8_t>(owner < 0 ? 0xFF : owner));
  w.u8(kEveryone);
  writeArgs(w, a);
  g_session->sendToAll(w, true);
  if (floor::slotInGame(0) && floor::slotLevel(0) == from)
  {
    floor::expectOwnership(owner == 0);
    execute(a, "Travelling with the party...");
  }
}

static void hostTick()
{
  if (!g_pending.active)
    return;
  uint8_t req = g_pending.requester;
  if (!floor::slotInGame(req) || floor::slotLevel(req) != g_pending.level)
  {
    cancelPending("requester left the floor");
    return;
  }
  if (distXZ(slotPosition(req), g_pending.spot) > kCancelRadius)
  {
    cancelPending("requester walked away");
    return;
  }

  // Everyone in the game counts, except a hardcore player who died (that death is final).
  // Players in town or elsewhere have to come back; dead players have to get up first.
  int total = 0, gathered = 0;
  std::string reason;
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
  {
    if (!floor::slotInGame(s))
      continue;
    bool hardcore = false;
    bool dead = slotDead(s, &hardcore);
    if (dead && hardcore)
      continue;
    total++;
    if (floor::slotLevel(s) != g_pending.level)
    {
      if (reason.empty())
        reason = "Waiting for " + nameOf(s) +
                 (floor::isTownLevel(floor::slotLevel(s)) ? " to come back from town" : " to come back");
    }
    else if (dead)
    {
      if (reason.empty())
        reason = "Waiting for " + nameOf(s) + " to resurrect";
    }
    else if (distXZ(slotPosition(s), g_pending.spot) <= kGatherRadius)
      gathered++;
  }

  float now = clock();
  if (total > 0 && gathered >= total)
  {
    if (g_pending.countdownStart < 0.0f)
    {
      g_pending.countdownStart = now;
      g_pending.lastCountdown = -1;
      TLMP_LOG("party: everyone gathered (%d); leaving in %d s", total, kCountdownSeconds);
    }
    int remaining = kCountdownSeconds - static_cast<int>(now - g_pending.countdownStart);
    if (remaining <= 0)
    {
      hostGo();
      return;
    }
    if (remaining != g_pending.lastCountdown)
    {
      g_pending.lastCountdown = remaining;
      sendStatus(true, gathered, total, remaining, "");
      if (floor::slotInGame(0) && floor::slotLevel(0) == g_pending.level)
        noticeCountdown(remaining);
    }
    return;
  }
  if (g_pending.countdownStart >= 0.0f)
  {
    TLMP_LOG("party: countdown stopped, %d/%d at the exit", gathered, total);
    g_pending.countdownStart = -1.0f;
    g_pending.lastGathered = -1; // force a waiting notice now
  }

  if (gathered != g_pending.lastGathered || reason != g_pending.lastReason || now >= g_pending.nextNotice)
  {
    g_pending.lastGathered = gathered;
    g_pending.lastReason = reason;
    g_pending.nextNotice = now + kNoticeInterval;
    sendStatus(true, gathered, total, 0, reason);
    if (floor::slotInGame(0) && floor::slotLevel(0) == g_pending.level)
      noticeWaiting(gathered, total, reason);
  }
}

// After "Continue" (any player, host included): go to the party's floor if it is somewhere else.
// Everyone in a dungeon is on the party's floor. After "Continue" that means going there; and if two
// players ever end up on different floors (both pressed Continue at once, each landing on their own
// saved floor), whoever isn't on the party's floor goes to it. Only once things have settled: during
// a party trip players arrive a few seconds apart, and the party floor moves with the first arrivals.
static void joinTick()
{
  constexpr float kSettle = 8.0f;
  static uint32_t myLevel = 0, partyLevel = 0;
  static float myLevelSince = 0.0f, partyLevelSince = 0.0f, lastJoin = -100.0f;
  engine::Level* level = currentLevel();
  if (!level || !localPlayer() || !g_enteredFromContinue)
    return;
  float now = clock();
  uint32_t here = levelId(level);
  const floor::PartyFloor& pf = floor::partyFloor();
  if (here != myLevel)
  {
    myLevel = here;
    myLevelSince = now;
  }
  if (pf.levelId != partyLevel)
  {
    partyLevel = pf.levelId;
    partyLevelSince = now;
  }
  if (floor::isTownLevel(here) || !floor::partyFloorElsewhere() || pf.levelId == here)
  {
    g_joinPending = false;
    return;
  }
  bool afterContinue = g_joinPending && now >= g_joinAt;
  bool settled = now - myLevelSince >= kSettle && now - partyLevelSince >= kSettle;
  if ((!afterContinue && !settled) || now - lastJoin < 20.0f)
    return;
  g_joinPending = false;
  lastJoin = now;
  joinPartyFloor("Joining the party...");
}

void attach(net::Session* session)
{
  g_session = session;
}

void tick()
{
  if (!connected())
    return;
  if (g_session->isHost())
    hostTick();
  joinTick();
}

void setRemoteRequester(int slot)
{
  g_remoteRequester = slot;
}

void beginEnterGame()
{
  g_enteringGame++;
}

bool enteredFromContinue()
{
  return g_enteredFromContinue;
}

void endEnterGame(bool fromContinue)
{
  g_enteringGame--;
  g_enteredFromContinue |= fromContinue;
  if (fromContinue && connected())
  {
    g_joinPending = true;
    g_joinAt = clock() + 2.0f; // let the loaded floor settle first
  }
}

// Asks the host for a party trip (the host itself files it directly).
static void requestTrip(uint8_t requester, const TravelArgs& args, const Vec3& spot)
{
  if (g_session->isHost())
  {
    hostRequest(requester, args, spot);
    return;
  }
  net::Writer w(Msg::TravelRequest);
  w.u8(requester);
  writeArgs(w, args);
  w.f32(spot.x);
  w.f32(spot.y);
  w.f32(spot.z);
  g_session->sendTo(0, w, true);
}

// A move for one player only (their town trip), sent by whoever applied their portal use.
static void orderSolo(uint8_t slot, const TravelArgs& args)
{
  net::Writer w(Msg::TravelGo);
  w.u32(floor::slotLevel(slot));
  w.u8(0xFF);
  w.u8(slot);
  writeArgs(w, args);
  floor::sendTo(slot, w, true);
}

bool allowChangeLevel(void* client, const TravelArgs& args)
{
  if (!connected() || g_executing || g_enteringGame > 0)
    return true; // agreed moves, loading a character, resurrecting (also "in town": a solo trip)
  if (triggers::isReplaying())
    return false; // replaying someone's door/portal use must never move this machine
  if (client != reinterpret_cast<void*>(gameClient()))
    return false; // a remote player's stand-in (the host registers them as clients)

  bool toTown = goesToTown(args);
  if (g_remoteRequester >= 0)
  {
    // We just applied another player's trigger use (e.g. their town portal): their move, not ours.
    auto slot = static_cast<uint8_t>(g_remoteRequester);
    if (toTown)
      orderSolo(slot, args);
    else
      requestTrip(slot, args, slotPosition(slot));
    TLMP_LOG("party: slot %u's trigger use asks for '%ls' (mode %d): %s", slot, args.dungeon.c_str(), args.mode,
             toTown ? "their own town trip" : "a party trip");
    return false;
  }
  if (toTown)
  {
    TLMP_LOG("party: going to town alone");
    floor::expectOwnership(false);
    return true;
  }
  if (floor::isTownLevel(levelId(currentLevel())) && floor::partyFloorElsewhere())
  {
    // Back from town: always to the floor the rest of the party is on.
    if (!joinPartyFloor("Returning to the party..."))
      showNotice("Can't find the party's floor yet", 3.0f);
    return false;
  }
  requestTrip(g_session->localSlot(), args, localPosition());
  if (!g_session->isHost())
    showNotice("Waiting for the party at the exit...", kNoticeInterval + 0.5f);
  return false;
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (!connected())
    return;
  bool host = g_session->isHost();
  if (host && type == Msg::TravelRequest)
  {
    uint8_t requester = r.u8();
    TravelArgs a = readArgs(r);
    Vec3 spot{r.f32(), r.f32(), r.f32()};
    if (r.ok() && requester < net::kMaxPlayers)
    {
      if (requester != fromSlot)
        TLMP_LOG("party: slot %u files a trip for slot %u (it applied their trigger use)", fromSlot, requester);
      hostRequest(requester, a, spot);
    }
    return;
  }
  if (type == Msg::TravelGo)
  {
    uint32_t from = r.u32();
    uint8_t owner = r.u8();
    uint8_t only = r.u8();
    TravelArgs a = readArgs(r);
    if (!r.ok())
      return;
    uint8_t me = g_session->localSlot();
    if (only != kEveryone && only != me)
      return;
    if (only == kEveryone && (host || levelId(currentLevel()) != from))
      return; // the host runs its own party moves; others elsewhere stay put
    floor::expectOwnership(owner == me);
    execute(a, only == kEveryone ? "Travelling with the party..." : nullptr);
    return;
  }
  if (!host && type == Msg::TravelStatus)
  {
    bool waiting = r.u8() != 0;
    int gathered = r.u8();
    int total = r.u8();
    int countdown = r.u8();
    std::string reason = r.str();
    if (!r.ok())
      return;
    if (!waiting)
      showNotice("Party travel cancelled", 3.0f);
    else if (countdown > 0)
      noticeCountdown(countdown);
    else
      noticeWaiting(gathered, total, reason);
  }
}

} // namespace tlmp::game::party
