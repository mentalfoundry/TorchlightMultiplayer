#include "game/spawns.h"
#include "game/engine.h"
#include "game/floor.h"
#include "game/game.h"
#include "game/internal.h"
#include "log.h"
#include "net/session.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tlmp::game::spawns {

constexpr float kNodeMatch = 0.5f; // same floor layout: a node is where the other machine's is
constexpr float kCatchUp = 15.0f;   // an event waits until everyone is this close to whoever set it off
constexpr float kNoticeInterval = 2.5f;

static net::Session* g_session = nullptr;

static void* g_collecting = nullptr;
static void* g_insidePlayer = nullptr;
static void* g_outsidePlayer = nullptr;
static unsigned g_inside = 0, g_outside = 0;

static bool inSession()
{
  return floor::slotInGame(0) || floor::slotInGame(1) || floor::slotInGame(2) || floor::slotInGame(3);
}

// --- trigger volumes ---------------------------------------------------------------------------

void beginTriggerUpdate(void* trigger)
{
  if (!inSession())
    return;
  g_collecting = trigger;
  g_insidePlayer = nullptr;
  g_outsidePlayer = nullptr;
  g_inside = g_outside = 0;
}

// The dead don't hold anything up (their stand-in lies wherever they fell).
static bool counts(void* player)
{
  if (!player)
    return false;
  int ai = engine::field<int>(player, engine::kUnitAIState);
  return ai != engine::kAIStateDying && ai != engine::kAIStateDead;
}

bool onTriggerInside(void* trigger, void* player)
{
  if (trigger != g_collecting)
    return true;
  if (counts(player))
  {
    g_inside++;
    if (!g_insidePlayer)
      g_insidePlayer = player;
  }
  return false;
}

bool onTriggerOutside(void* trigger, void* player)
{
  if (trigger != g_collecting)
    return true;
  if (counts(player))
  {
    g_outside++;
    g_outsidePlayer = player;
  }
  return false;
}

static std::string nameOf(uint8_t slot)
{
  if (slot == g_session->localSlot())
  {
    std::string n = localPlayerInfo().charName;
    return n.empty() ? "you" : n;
  }
  const net::RemotePlayer& p = g_session->player(slot);
  return displayName(slot, p.info.charName.empty() ? p.name : p.info.charName);
}

// Who an event waits for: everyone in the game, wherever they are, except a hardcore player who
// died (that is final) and players on the title screen.
static bool required(uint8_t slot, bool* dead)
{
  if (!floor::slotInGame(slot))
    return false;
  bool hardcore;
  if (slot == g_session->localSlot())
  {
    uintptr_t me = localPlayer();
    int ai = engine::field<int>(reinterpret_cast<void*>(me), engine::kUnitAIState);
    *dead = ai == engine::kAIStateDying || ai == engine::kAIStateDead;
    hardcore = engine::field<uint8_t>(reinterpret_cast<void*>(me), engine::kPlayerHardcore) != 0;
  }
  else
  {
    const net::PlayerState& st = g_session->player(slot).state;
    *dead = st.dead;
    hardcore = st.hardcore;
  }
  return !(*dead && hardcore);
}

static Vec3 slotPosition(uint8_t slot)
{
  if (slot == g_session->localSlot())
    return unitPosition(reinterpret_cast<engine::Unit*>(localPlayer()));
  const net::PlayerState& st = g_session->player(slot).state;
  return Vec3{st.pos[0], st.pos[1], st.pos[2]};
}

static void sendWaiting(uint8_t missing)
{
  net::Writer w(net::Msg::EventWait);
  w.u8(missing);
  if (g_session->isHost())
    g_session->sendToAll(w, true);
  else
    g_session->sendTo(0, w, true);
}

void endTriggerUpdate(void* trigger, HandlerFn inside, HandlerFn outside)
{
  if (trigger != g_collecting)
    return;
  g_collecting = nullptr;
  if (!g_insidePlayer)
  {
    if (g_outsidePlayer)
      outside(trigger, g_outsidePlayer);
    return;
  }
  uint32_t here = levelId(currentLevel());
  if (floor::isTownLevel(here))
  {
    inside(trigger, g_insidePlayer); // town events are anyone's, as in single player
    return;
  }

  // Someone stepped in. The event happens once the whole party is close by; until then the area
  // stays "outside", so nothing fires.
  Vec3 at = unitPosition(static_cast<engine::Unit*>(g_insidePlayer));
  uint8_t missing = 0;
  std::string waitingFor;
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
  {
    bool dead = false;
    if (!required(s, &dead))
      continue;
    const char* why = nullptr;
    if (floor::slotLevel(s) != here)
      why = floor::isTownLevel(floor::slotLevel(s)) ? " (in town)" : " (on another floor)";
    else if (dead)
      why = " (to resurrect)";
    else if (distXZ(slotPosition(s), at) > kCatchUp)
      why = "";
    if (!why)
      continue;
    missing |= static_cast<uint8_t>(1u << s);
    if (!waitingFor.empty())
      waitingFor += ", ";
    waitingFor += nameOf(s) + why;
  }
  if (!missing)
  {
    inside(trigger, g_insidePlayer);
    return;
  }

  static float nextNotice = 0.0f;
  if (clock() < nextNotice)
    return;
  nextNotice = clock() + kNoticeInterval;
  bool meWaiting = !(missing & (1u << g_session->localSlot()));
  if (meWaiting)
    showNotice("Something is about to happen. Waiting for " + waitingFor + " to catch up", kNoticeInterval + 0.5f);
  if (floor::ownsCurrentLevel())
    sendWaiting(missing); // the floor's owner tells everyone it is waiting for (wherever they are)
}

void attach(net::Session* session)
{
  g_session = session;
}

void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r)
{
  if (type != net::Msg::EventWait || !g_session || !g_session->isConnected())
    return;
  uint8_t missing = r.u8();
  if (!r.ok())
    return;
  if (g_session->isHost())
  {
    net::Writer w(net::Msg::EventWait);
    w.u8(missing);
    for (uint8_t s = 1; s < net::kMaxPlayers; s++)
      if (s != fromSlot && (missing & (1u << s)))
        g_session->sendTo(s, w, true);
  }
  if (missing & (1u << g_session->localSlot()))
    showNotice("The party is waiting for you to catch up before something happens", kNoticeInterval + 0.5f);
}

// --- formation nodes ---------------------------------------------------------------------------

template <typename Fn>
static void forEachNode(engine::Level* level, Fn&& fn)
{
  if (!level)
    return;
  auto** nodes = engine::field<uint8_t**>(level, engine::kLevelFormationNodes);
  uint32_t count = engine::field<uint32_t>(level, engine::kLevelFormationNodes + 4);
  for (uint32_t i = 0; nodes && i < count && i < 100000; i++)
    if (nodes[i])
      fn(nodes[i]);
}

static Vec3 nodePosition(uint8_t* node)
{
  Vec3 out{};
  engine::GetPosition(node, &out, 1);
  return out;
}

static unsigned g_nodeUpdates = 0;

void afterNodeUpdate(void* node, uint32_t a, uint32_t b, NodeUpdateFn original)
{
  if (++g_nodeUpdates == 1 || g_nodeUpdates % 5000 == 0)
    TLMP_LOG("spawns: formation node updates seen: %u", g_nodeUpdates);
  // `node` is the node's level-update part (CFormationNode + 0xF0); its "spawned" flag follows.
  auto* spawned = static_cast<uint8_t*>(node) + 4;
  if (*spawned || !floor::ownsCurrentLevel() || !inSession())
    return;
  uint32_t lid = levelId(currentLevel());
  for (uint8_t s = 0; s < net::kMaxPlayers && !*spawned; s++)
  {
    if (!floor::slotInGame(s) || floor::slotLevel(s) != lid || floor::currentOwner() == s)
      continue;
    engine::Unit* standIn = avatarOf(s);
    if (!standIn)
      continue;
    Vec3 p = unitPosition(standIn);
    original(node, 0, b, &p); // no time passes on these extra checks
    if (*spawned)
      TLMP_LOG("spawns: a monster group woke up for slot %u", s);
  }
  (void)a;
}

void writeSnapshot(net::Writer& w)
{
  std::vector<Vec3> waiting;
  forEachNode(currentLevel(), [&](uint8_t* node) {
    if (!node[engine::kNodeSpawned])
      waiting.push_back(nodePosition(node));
  });
  w.u16(static_cast<uint16_t>(waiting.size()));
  for (const Vec3& p : waiting)
  {
    w.f32(p.x);
    w.f32(p.y);
    w.f32(p.z);
  }
}

void applySnapshot(net::Reader& r)
{
  uint16_t n = r.u16();
  std::vector<Vec3> waiting;
  for (uint16_t i = 0; i < n && r.ok(); i++)
  {
    Vec3 p;
    p.x = r.f32();
    p.y = r.f32();
    p.z = r.f32();
    waiting.push_back(p);
  }
  // Monster groups are placed differently on each machine (formation nodes don't match by position
  // across games; seen in tests), so the previous owner's list can't be mapped onto ours: each
  // machine keeps its own groups. Read for the snapshot format only.
  (void)waiting;
}

bool nearestWaitingGroup(float minDistance, float* x, float* z)
{
  engine::Unit* me = reinterpret_cast<engine::Unit*>(localPlayer());
  if (!me)
    return false;
  Vec3 from = unitPosition(me);
  float best = 1e9f;
  bool found = false;
  forEachNode(currentLevel(), [&](uint8_t* node) {
    if (node[engine::kNodeSpawned])
      return;
    Vec3 p = nodePosition(node);
    float d = distXZ(p, from);
    if (d >= minDistance && d < best)
    {
      best = d;
      *x = p.x;
      *z = p.z;
      found = true;
    }
  });
  return found;
}

void listWaiting(void (*fn)(float x, float z, void* ctx), void* ctx)
{
  forEachNode(currentLevel(), [&](uint8_t* node) {
    if (!node[engine::kNodeSpawned])
    {
      Vec3 p = nodePosition(node);
      fn(p.x, p.z, ctx);
    }
  });
}

void countGroups(unsigned* total, unsigned* waiting)
{
  *total = *waiting = 0;
  forEachNode(currentLevel(), [&](uint8_t* node) {
    (*total)++;
    *waiting += node[engine::kNodeSpawned] ? 0 : 1;
  });
}

void afterActivityCheck(void* unit, ActivityFn original)
{
  auto* active = static_cast<uint8_t*>(unit) + engine::kUnitActive;
  if (*active || !floor::ownsCurrentLevel() || !inSession())
    return;
  // Monsters near any player on the floor live, not just near this machine's own player.
  uint32_t lid = levelId(currentLevel());
  for (uint8_t s = 0; s < net::kMaxPlayers && !*active; s++)
  {
    if (!floor::slotInGame(s) || floor::slotLevel(s) != lid)
      continue;
    engine::Unit* standIn = avatarOf(s);
    if (!standIn)
      continue;
    Vec3 p = unitPosition(standIn);
    original(unit, &p);
  }
}

} // namespace tlmp::game::spawns
