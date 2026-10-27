#include "game/combat.h"
#include "game/floor.h"
#include "game/internal.h"
#include "game/monsters.h"
#include "log.h"
#include "net/session.h"

namespace tlmp::game::combat {

using net::Msg;

constexpr float kShareRadius = 50.0f; // kills within this distance of a player give them XP and fame
constexpr unsigned kDetailedLogs = 20; // log the first hits of each kind in full

static net::Session* g_session = nullptr;
static engine::Unit* g_applyingTo = nullptr; // target of our own TakeDamage call (not re-routed)
static bool g_granting = false;             // our own GiveExperience / GiveFame calls

enum AttackerKind : uint8_t
{
  kByPlayer = 0,
  kByPet = 1,
  kByMinion = 2, // summons
  kByOther = 3,  // no attacker given (some missiles and effects)
};

enum VictimKind : uint8_t
{
  kVictimPlayer = 0,
  kVictimPet = 1,
};

static const char* const kAttackerNames[] = {"player", "pet", "summon", "other"};

struct Counters
{
  unsigned sent = 0;      // client: hits on copies sent to the host
  unsigned applied = 0;   // host: client hits applied to real monsters
  unsigned forwarded = 0; // host: hits on stand-ins sent to their owners
  unsigned taken = 0;     // client: host monster hits applied to us
  unsigned dropped = 0;   // either: arrived for another floor / unknown unit
  unsigned kills = 0;     // rewards given for kills by others
};
static Counters g_count;

static bool connected()
{
  return g_session && g_session->isConnected();
}

static void writeHit(net::Writer& w, const Hit& h)
{
  engine::Vec3 dir = h.dir ? *h.dir : engine::Vec3{0.0f, 0.0f, 0.0f};
  w.f32(h.damage);
  w.f32(dir.x);
  w.f32(dir.y);
  w.f32(dir.z);
  w.f32(h.force);
  w.u8(static_cast<uint8_t>(h.showNumber));
  w.u8(static_cast<uint8_t>(h.react));
  w.u8(static_cast<uint8_t>(h.knockback));
  w.u32(static_cast<uint32_t>(h.arg11));
  w.u32(h.flags);
}

// dir is stored by the caller; h.dir points at it.
static void readHit(net::Reader& r, Hit& h, engine::Vec3& dir)
{
  h.damage = r.f32();
  dir.x = r.f32();
  dir.y = r.f32();
  dir.z = r.f32();
  h.dir = &dir;
  h.force = r.f32();
  h.showNumber = static_cast<char>(r.u8());
  h.react = static_cast<char>(r.u8());
  h.knockback = static_cast<char>(r.u8());
  h.arg11 = static_cast<int>(r.u32());
  h.flags = r.u32();
}

static void applyHit(engine::Unit* target, const Hit& h)
{
  engine::Unit* previous = g_applyingTo;
  g_applyingTo = target;
  engine::TakeDamage(target, h.level, h.sourcePos, h.damage, h.dir, h.force, h.attacker, h.showNumber, h.react,
                     h.knockback, h.arg11, h.flags);
  g_applyingTo = previous;
}

static bool isMinionOf(uintptr_t player, const void* unit)
{
  if (!player || !unit)
    return false;
  auto** it = engine::field<void**>(reinterpret_cast<void*>(player), engine::kPlayerMinions);
  auto** end = engine::field<void**>(reinterpret_cast<void*>(player), engine::kPlayerMinions + 4);
  for (; it && it != end; ++it)
    if (*it == unit)
      return true;
  return false;
}

// --- client: our hits on the host's monsters --------------------------------------------------

static bool clientHitsCopy(engine::Unit* target, Hit& hit)
{
  uintptr_t me = localPlayer();
  auto attacker = reinterpret_cast<uintptr_t>(hit.attacker);
  // Copies (monsters and remote players alike) only show what happens on other machines.
  if (attacker && (monsters::isPuppetUnit(hit.attacker) || isAvatarUnit(hit.attacker)))
    return false;
  uint8_t kind = !attacker                     ? kByOther
                 : attacker == me              ? kByPlayer
                 : attacker == localPet()      ? kByPet
                 : isMinionOf(me, hit.attacker) ? kByMinion
                                                : kByOther;

  int owner = floor::currentOwner();
  if (uint32_t id = monsters::puppetId(target); id && owner >= 0)
  {
    net::Writer w(Msg::MonsterDamage);
    w.u32(levelId(currentLevel()));
    w.u32(id);
    w.u8(kind);
    writeHit(w, hit);
    floor::sendTo(static_cast<uint8_t>(owner), w, true);
    if (++g_count.sent <= kDetailedLogs)
      TLMP_LOG("combat: [client] %s hit monster %u for %.1f (flags %x, react %d, knockback %d) -> host",
               kAttackerNames[kind], id, hit.damage, hit.flags, hit.react, hit.knockback);
  }

  // Locally the hit lands (reaction, knockback) but never to 0 health: only the host's monster
  // dies, and the copy dies when the host says so.
  float room = engine::field<float>(target, engine::kUnitHealth) - 1.0f;
  if (room <= 0.0f)
    return false;
  if (hit.damage > room)
    hit.damage = room;
  return true;
}

static void onPlayerDamage(net::Reader& r)
{
  uint32_t lid = r.u32();
  uint8_t victimKind = r.u8();
  uint32_t attackerId = r.u32();
  Hit hit;
  engine::Vec3 dir{};
  readHit(r, hit, dir);
  engine::Level* level = currentLevel();
  if (!r.ok() || !level || lid != levelId(level))
  {
    g_count.dropped++;
    return;
  }
  auto* victim = reinterpret_cast<engine::Unit*>(victimKind == kVictimPet ? localPet() : localPlayer());
  if (!victim)
  {
    g_count.dropped++;
    return;
  }
  engine::Unit* attacker = monsters::puppetById(attackerId);
  engine::Vec3 source = unitPosition(attacker ? attacker : victim);
  hit.level = level;
  hit.sourcePos = &source;
  hit.attacker = attacker;
  if (++g_count.taken <= kDetailedLogs)
    TLMP_LOG("combat: [client] host monster %u (copy %p) hit my %s for %.1f (flags %x)", attackerId, attacker,
             victimKind == kVictimPet ? "pet" : "character", hit.damage, hit.flags);
  applyHit(victim, hit);
}

// --- host: clients' hits, and hits on stand-ins ------------------------------------------------

static bool hostHitsStandIn(engine::Unit* target, const Hit& hit)
{
  bool isPet = false;
  int slot = avatarSlotOf(target, &isPet);
  if (slot < 0)
    return true;
  int ai = engine::field<int>(target, engine::kUnitAIState);
  if (ai == engine::kAIStateDying || ai == engine::kAIStateDead)
    return false; // its player is dead already
  uint32_t attackerId = monsters::trackedId(hit.attacker);
  net::Writer w(Msg::PlayerDamage);
  w.u32(levelId(currentLevel()));
  w.u8(isPet ? kVictimPet : kVictimPlayer);
  w.u32(attackerId);
  writeHit(w, hit);
  floor::sendTo(static_cast<uint8_t>(slot), w, true);
  if (++g_count.forwarded <= kDetailedLogs)
    TLMP_LOG("combat: [host] monster %u (%p) hit slot %d's %s for %.1f -> owner", attackerId, hit.attacker, slot,
             isPet ? "pet" : "character", hit.damage);
  return false; // the stand-in itself is never hurt
}

static void onMonsterDamage(uint8_t slot, net::Reader& r)
{
  uint32_t lid = r.u32();
  uint32_t id = r.u32();
  uint8_t kind = r.u8();
  Hit hit;
  engine::Vec3 dir{};
  readHit(r, hit, dir);
  engine::Level* level = currentLevel();
  engine::Unit* monster = r.ok() && level && lid == levelId(level) ? monsters::trackedById(id) : nullptr;
  if (!monster)
  {
    g_count.dropped++;
    return;
  }
  engine::Unit* attacker = kind == kByPet && avatarPetOf(slot) ? avatarPetOf(slot) : avatarOf(slot);
  engine::Vec3 source = unitPosition(attacker ? attacker : monster);
  hit.level = level;
  hit.sourcePos = &source;
  hit.attacker = attacker;
  hit.flags |= engine::kDamageNoReflect; // the client already took any reflected damage itself
  if (++g_count.applied <= kDetailedLogs)
    TLMP_LOG("combat: [host] slot %u's %s hit monster %u for %.1f (health %.1f, attacker %p)", slot,
             kind < 4 ? kAttackerNames[kind] : "?", id, hit.damage, engine::field<float>(monster, engine::kUnitHealth),
             attacker);
  applyHit(monster, hit);
}

// --- public ------------------------------------------------------------------------------------

void attach(net::Session* session)
{
  g_session = session;
}

void tick()
{
  static float next = 0.0f;
  static Counters last;
  if (clock() < next)
    return;
  next = clock() + 10.0f;
  const Counters& c = g_count;
  if (c.sent == last.sent && c.applied == last.applied && c.forwarded == last.forwarded && c.taken == last.taken &&
      c.dropped == last.dropped && c.kills == last.kills)
    return;
  last = c;
  TLMP_LOG("combat: hits sent %u, applied %u, forwarded %u, taken %u, dropped %u; shared kills %u", c.sent, c.applied,
           c.forwarded, c.taken, c.dropped, c.kills);
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (!connected())
    return;
  if (type == Msg::MonsterDamage && floor::ownsCurrentLevel())
    onMonsterDamage(fromSlot, r);
  else if (type == Msg::PlayerDamage)
    onPlayerDamage(r);
}

bool beforeTakeDamage(engine::Unit* target, Hit& hit)
{
  // Zero-damage calls (some effects, flags 0x200) are no-ops in TakeDamage itself.
  if (!connected() || !target || target == g_applyingTo || !(hit.damage > 0.0f))
    return true;
  if (floor::ownsCurrentLevel())
    return hostHitsStandIn(target, hit);
  if (monsters::isPuppetUnit(target))
    return clientHitsCopy(target, hit);
  if (isAvatarUnit(target))
    return false; // remote players get hurt on their own machine
  return true;
}

bool allowReward(engine::Unit* unit, engine::Level* level, engine::Unit* victim, int amount, char raw, bool fame)
{
  if (!connected() || g_granting)
    return true;
  if (!floor::ownsCurrentLevel())
  {
    // Rewards for the owner's monsters come only from onHostKill.
    return !(victim && monsters::isPuppetUnit(victim));
  }
  if (!isAvatarUnit(unit))
    return true;
  // A remote player's stand-in made the kill. Its owner is rewarded on their own machine; the
  // host player gets the share of a party kill instead of the stand-in.
  uintptr_t me = localPlayer();
  if (me && victim && distXZ(unitPosition(reinterpret_cast<engine::Unit*>(me)), unitPosition(victim)) <= kShareRadius)
  {
    g_granting = true;
    (fame ? engine::GiveFame : engine::GiveExperience)(reinterpret_cast<engine::Unit*>(me), level, victim, amount, raw);
    g_granting = false;
    if (!fame)
    {
      g_count.kills++;
      TLMP_LOG("combat: [host] a remote player's kill gave me %d XP", amount);
    }
  }
  return false;
}

bool onHostKill(engine::Unit* puppet)
{
  uintptr_t me = localPlayer();
  engine::Level* level = currentLevel();
  if (!me || !level || !connected() || floor::ownsCurrentLevel())
    return false;
  float distance = distXZ(unitPosition(reinterpret_cast<engine::Unit*>(me)), unitPosition(puppet));
  int xp = engine::field<int>(puppet, engine::kUnitXpReward);
  int fame = engine::field<int>(puppet, engine::kUnitFameReward);
  if (distance > kShareRadius)
  {
    TLMP_LOG("combat: [client] kill %.0f away: too far for a share (%d XP)", distance, xp);
    return false;
  }
  g_granting = true;
  if (xp)
    engine::GiveExperience(reinterpret_cast<engine::Unit*>(me), level, puppet, xp, 0);
  if (fame)
    engine::GiveFame(reinterpret_cast<engine::Unit*>(me), level, puppet, fame, 0);
  g_granting = false;
  // As TakeDamage does after paying out: a copy pays once.
  *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(puppet) + engine::kUnitXpReward) = 0;
  *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(puppet) + engine::kUnitFameReward) = 0;
  g_count.kills++;
  TLMP_LOG("combat: [client] host kill %.0f away gave me %d XP, %d fame", distance, xp, fame);
  return true;
}

int killerFor(engine::Unit* victim, int killer)
{
  if (!connected() || !killer || !floor::ownsCurrentLevel() || !isAvatarUnit(reinterpret_cast<void*>(killer)))
    return killer;
  // A remote player's stand-in made the kill: their own game drops their loot and counts it for their
  // quests. Here it is the local player's kill if they are close enough to share in it, else nobody's.
  uintptr_t me = localPlayer();
  if (me && distXZ(unitPosition(reinterpret_cast<engine::Unit*>(me)), unitPosition(victim)) <= kShareRadius)
    return static_cast<int>(me);
  return 0;
}

} // namespace tlmp::game::combat
