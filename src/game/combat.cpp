#include "game/combat.h"
#include "game/floor.h"
#include "game/internal.h"
#include "game/minions.h"
#include "game/monsters.h"
#include "game/skills.h"
#include "log.h"
#include "net/session.h"

#include <cmath>

namespace tlmp::game::combat {

using net::Msg;

constexpr float kShareRadius = 50.0f; // kills within this distance of a player give them XP and fame
constexpr unsigned kDetailedLogs = 20; // log the first hits of each kind in full

static net::Session* g_session = nullptr;
static net::PartyRules g_rules;
static Share g_lastShare;
static float g_lastDamageScale = 0.0f;
static unsigned g_sharesGiven = 0;
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
  kVictimMinion = 2, // + the minion's index in its player's report (minions.cpp)
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
  unsigned showOnly = 0;  // host: hits by stand-ins' own skills or summons, dropped
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
  // Copies (monsters, remote players and their summons) only show what happens on other machines.
  if (attacker && (monsters::isPuppetUnit(hit.attacker) || skills::isShowOnly(hit.attacker)))
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
  auto* victim = victimKind >= kVictimMinion ? minions::localMinion(static_cast<uint8_t>(victimKind - kVictimMinion))
                 : reinterpret_cast<engine::Unit*>(victimKind == kVictimPet ? localPet() : localPlayer());
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
             victimKind >= kVictimMinion ? "minion" : victimKind == kVictimPet ? "pet" : "character", hit.damage, hit.flags);
  applyHit(victim, hit);
}

// --- host: clients' hits, and hits on stand-ins ------------------------------------------------

static bool hostHitsStandIn(engine::Unit* target, const Hit& hit)
{
  bool isPet = false;
  int slot = avatarSlotOf(target, &isPet);
  uint8_t victimKind = isPet ? kVictimPet : kVictimPlayer;
  if (slot < 0)
  {
    // A copy of a remote player's minion: the hit goes to the real minion (a summon not matched
    // to one yet takes nothing).
    uint8_t mSlot = 0, index = 0;
    if (!minions::copyOf(target, &mSlot, &index))
      return true;
    if (index == 0xFF)
      return false;
    slot = mSlot;
    victimKind = static_cast<uint8_t>(kVictimMinion + index);
  }
  int ai = engine::field<int>(target, engine::kUnitAIState);
  if (ai == engine::kAIStateDying || ai == engine::kAIStateDead)
    return false; // its player is dead already
  uint32_t attackerId = monsters::trackedId(hit.attacker);
  net::Writer w(Msg::PlayerDamage);
  w.u32(levelId(currentLevel()));
  w.u8(victimKind);
  w.u32(attackerId);
  writeHit(w, hit);
  floor::sendTo(static_cast<uint8_t>(slot), w, true);
  if (++g_count.forwarded <= kDetailedLogs)
    TLMP_LOG("combat: [host] monster %u (%p) hit slot %d's %s for %.1f -> owner", attackerId, hit.attacker, slot,
             victimKind >= kVictimMinion ? "minion" : isPet ? "pet" : "character", hit.damage);
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

void setRules(const net::PartyRules& rules)
{
  g_rules = rules;
  TLMP_LOG("combat: monster damage +%.2f%% per extra player; XP %s%s", rules.damagePerPlayer * 100.0f,
           rules.experience == net::ExperienceModel::Diablo2 ? "Diablo 2 style" : "in full to everyone nearby",
           rules.experience == net::ExperienceModel::Diablo2 && rules.levelGap ? " with the level gap" : "");
}

Share lastShare()
{
  return g_lastShare;
}

float lastDamageScale()
{
  return g_lastDamageScale;
}

unsigned sharesGiven()
{
  return g_sharesGiven;
}

static int levelOf(const void* unit)
{
  return unit ? engine::field<int>(const_cast<void*>(unit), engine::kUnitLevel) : 0;
}

// Diablo 2's level gap: 100% of the share within 5 levels of the monster, 5% from 10 levels on,
// linear between (6: 81%, 7: 62%, 8: 43%, 9: 24%).
static float gapFactor(int gap)
{
  if (gap < 0)
    gap = -gap;
  if (gap <= 5)
    return 1.0f;
  if (gap >= 10)
    return 0.05f;
  return 1.0f - 0.19f * static_cast<float>(gap - 5);
}

int playersNear(engine::Unit* victim, int* levels)
{
  int n = 1;
  int sum = levelOf(reinterpret_cast<void*>(localPlayer()));
  Vec3 at = unitPosition(victim);
  for (uint8_t slot = 0; victim && slot < net::kMaxPlayers; slot++)
  {
    engine::Unit* other = avatarOf(slot);
    if (!other || distXZ(unitPosition(other), at) > kShareRadius)
      continue;
    n++;
    sum += levelOf(other);
  }
  if (levels)
    *levels = sum;
  return n;
}

// The local player's XP for a kill worth `amount` (PartyRules). Near: this player, and every other
// player whose character is on this floor within kShareRadius of the victim.
static int localShare(engine::Unit* victim, int amount)
{
  uintptr_t me = localPlayer();
  if (!connected() || !me || !victim || amount <= 0 || g_rules.experience != net::ExperienceModel::Diablo2)
    return amount;
  int myLevel = levelOf(reinterpret_cast<void*>(me));
  int levels = 0;
  int nearby = playersNear(victim, &levels);
  float total = static_cast<float>(amount) * monsters::healthScale() * (nearby > 1 ? 1.35f : 1.0f);
  float part = levels > 0 ? static_cast<float>(myLevel) / static_cast<float>(levels) : 1.0f / nearby;
  float gap = g_rules.levelGap ? gapFactor(myLevel - levelOf(victim)) : 1.0f;
  int given = static_cast<int>(std::lround(total * part * gap));
  if (given < 1)
    given = 1;
  g_lastShare = Share{amount, given, floor::playersInGame(), nearby, gap, true};
  g_sharesGiven++;
  static unsigned logged = 0;
  if (++logged <= kDetailedLogs)
    TLMP_LOG("combat: kill worth %d XP -> %d for me (%d in the game x%.2f, %d nearby%s, my level %d of %d, monster level %d: %.0f%%)",
             amount, given, floor::playersInGame(), monsters::healthScale(), nearby, nearby > 1 ? " +35%" : "", myLevel,
             levels, levelOf(victim), gap * 100.0f);
  return given;
}

// Diablo 2: monsters hit harder for every player in the game beyond the first.
static void scaleMonsterHit(engine::Unit* target, Hit& hit)
{
  if (g_rules.damagePerPlayer <= 0.0f || !hit.attacker || !monsters::isHostile(hit.attacker) ||
      monsters::isHostile(target))
    return;
  int extra = floor::playersInGame() - 1;
  if (extra <= 0)
    return;
  float scale = 1.0f + g_rules.damagePerPlayer * static_cast<float>(extra);
  if (g_lastDamageScale == 0.0f)
    TLMP_LOG("combat: monster hits x%.4f (%d players in the game)", scale, extra + 1);
  g_lastDamageScale = scale;
  hit.damage *= scale;
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
      c.dropped == last.dropped && c.showOnly == last.showOnly && c.kills == last.kills)
    return;
  last = c;
  TLMP_LOG("combat: hits sent %u, applied %u, forwarded %u, taken %u, dropped %u, show-only %u; shared kills %u", c.sent,
           c.applied, c.forwarded, c.taken, c.dropped, c.showOnly, c.kills);
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
  {
    // Monsters run here: their hits are scaled here, once (forwarded ones go out scaled).
    scaleMonsterHit(target, hit);
    // A stand-in's own skills (and its summons) are for show: the real player's hits come from
    // their machine (onMonsterDamage applies those, and they pass above).
    if (skills::isShowOnly(hit.attacker))
    {
      if (++g_count.showOnly <= kDetailedLogs)
        TLMP_LOG("combat: [host] dropped a show-only hit for %.1f by %p (a stand-in's own skill or summon)", hit.damage,
                 hit.attacker);
      return false;
    }
    return hostHitsStandIn(target, hit);
  }
  if (monsters::isPuppetUnit(target))
    return clientHitsCopy(target, hit);
  if (isAvatarUnit(target))
    return false; // remote players get hurt on their own machine
  return true;
}

bool allowReward(engine::Unit* unit, engine::Level* level, engine::Unit* victim, int& amount, char raw, bool fame)
{
  if (!connected() || g_granting)
    return true;
  // The local player's own kill (or its pet's or minions', forwarded to it), on the floor it runs.
  if (!fame && !raw && reinterpret_cast<uintptr_t>(unit) == localPlayer() && floor::ownsCurrentLevel() &&
      victim && monsters::isHostile(victim))
    amount = localShare(victim, amount);
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
    int share = fame || raw ? amount : localShare(victim, amount);
    g_granting = true;
    (fame ? engine::GiveFame : engine::GiveExperience)(reinterpret_cast<engine::Unit*>(me), level, victim, share, raw);
    g_granting = false;
    if (!fame)
    {
      g_count.kills++;
      TLMP_LOG("combat: [host] a remote player's kill gave me %d XP", share);
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
  if (xp)
    xp = localShare(puppet, xp);
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
