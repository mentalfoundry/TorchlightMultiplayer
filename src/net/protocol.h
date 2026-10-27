#pragma once

// Wire protocol. Everything is little-endian and explicitly serialized through
// Writer/Reader -- never memcpy a struct onto the wire.
//
// Topology: star. The host is slot 0 and is authoritative for world state; clients
// only talk to the host, which relays what other clients need.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tlmp::net {

constexpr uint32_t kMagic = 0x504D4C54; // "TLMP"
constexpr uint16_t kProtocolVersion = 25;
constexpr int kMaxPlayers = 4;
constexpr uint8_t kInvalidSlot = 0xFF;
constexpr int kEquipSlots = 12; // CInventory slots 0..11 are the equipment slots

enum Channel : uint8_t
{
  kChanReliable = 0, // ordered + reliable: control, events
  kChanState = 1,    // unsequenced-ish snapshots, newest wins
  kChanCount = 2,
};

enum class Msg : uint8_t
{
  Hello = 1,    // C->H: magic, version, exeStamp, name
  Welcome,      // H->C: yourSlot, [slot, name]...
  Reject,       // H->C: reason string, then disconnect
  PlayerJoined, // H->C: slot, name
  PlayerLeft,   // H->C: slot
  PlayerState,  // C->H and H->C: slot, tick, PlayerState
  Chat,         // both: slot, text
  PlayerInfo,   // both: slot, PlayerInfo -- reliable, sent on change; host relays and
                // replays all known infos to a newly welcomed client
  WorldSeed,    // H->C: u32 host character's world seed (client+0xFB0); sent on welcome
                // and on change. Clients adopt it so floors generate identically.

  // Host-authoritative monsters (game/monsters.cpp). All carry the host's levelId first;
  // clients on another level ignore them.
  MonsterRoster, // H->C reliable, ~1 Hz: every hostile monster: id, UNIT_GUID, level, dead, spawn position
  MonsterState,  // H->C unreliable, ~10 Hz: nearby monsters: id, position, health, AI state
  MonsterDied,   // H->C reliable: id

  // Doors and other openables (game/triggers.cpp), identified by UNIT_GUID + position.
  TriggerUse,    // both, reliable: levelId, guid, x, z, new state (host applies and relays)
  TriggerRoster, // H->C reliable, ~0.5 Hz: levelId, [guid, x, z, state] for moved triggers
  TriggerRequest, // C->H reliable: levelId, guid, x, z -- a client used a trigger; the host does it

  // Party travel (game/party.cpp): map changes happen only when everyone is at the exit.
  TravelRequest, // C->H reliable: ChangeLevel arguments + where the requester stands
  TravelStatus,  // H->C reliable: waiting?, gathered, total, countdown seconds (0 = still gathering)
  TravelGo,      // H->C reliable: from levelId, owner slot of the destination, ChangeLevel arguments --
                 // run by everyone on that level
  HostLocation,  // (unused since PartyFloor)

  // Combat across games (game/combat.cpp). TakeDamage arguments travel as they were.
  MonsterDamage, // C->H reliable: levelId, monster id, attacker kind, hit -- a client hit one of the host's monsters
  PlayerDamage,  // H->C reliable: levelId, victim kind, attacking monster id, hit -- a host monster hit your avatar

  // Animations (game/anim.cpp): one-shot animations (attacks, casts) of players, pets and host
  // monsters, replayed on their copies by animation name.
  AnimPlay, // both, reliable: slot, levelId, kind, monster id, call, name, args (host relays players')

  // Items players drop (game/loot.cpp). Kill and chest loot stays per player; items a player
  // drops from their inventory are shared, and the host decides who picks each one up.
  ItemDrop,  // C->H reliable: key, levelId, position, GUID, saved item state
  ItemSpawn, // H->C reliable: key, levelId, position, GUID, saved item state (to everyone but the dropper)
  ItemClaim, // C->H reliable: key -- I want to pick it up
  ItemTaken, // H->C reliable: key, slot -- it's theirs; everyone else removes their copy

  // Floors (game/floor.cpp). The party's dungeon floor is run ("owned") by exactly one machine:
  // the host if it is there, else a client who is. Town needs no owner.
  Relay,         // both: dest slot (0xFE = everyone on a level), from slot, levelId, payload message --
                 // lets a client that owns a floor reach the others through the host
  Location,      // C->H reliable, on change: levelId, level name, dungeon name, floor
  PartyFloor,    // H->C reliable, on change and ~0.5 Hz: levelId (0 = none), level name, dungeon, floor, owner slot
  OwnerGrant,    // H->C reliable: levelId, snapshot -- you run this floor now
  OwnerRelease,  // H->C reliable: levelId -- hand the floor back: send a snapshot, keep copies only
  FloorSnapshot, // C->H reliable: levelId, snapshot -- the floor's monsters and doors as the owner left them

  MonsterLook,   // owner->floor reliable: levelId, id, the monster's save state (copies are built from it)
  EventWait,     // owner->everyone (via host) reliable: slot mask -- an event on the party floor waits for them
  TestSync,      // both, reliable (test mode only): slot, barrier name -- the host relays it to everyone
  QuestSpawn,    // ->floor owner reliable: levelId, position, saved unit state -- a monster my quest places here
  SkillCast,     // ->floor reliable: levelId, skill GUID, level, name, aim point, target id -- I started a skill
  Minions,       // ->floor unreliable: levelId, count, per minion GUID, position, health, max, dead -- mine
};

enum class RejectReason : uint8_t
{
  Full = 1,
  BadVersion,
  BadGameBuild,
};

// Slow-changing description of a player's character, sent reliably on change.
struct PlayerInfo
{
  std::string className; // "Destroyer", "Vanquisher", "Alchemist"
  std::string petType;   // MONSTERS table name, e.g. "Dog" (for logs; may be empty, see petGuid)
  std::string charName;  // the character's name as shown over its head
  std::string petName;
  uint64_t petGuid = 0;  // UNIT_GUID of the pet's definition; 0 if no pet
  uint16_t petLevel = 0;
  std::array<uint64_t, kEquipSlots> equipment{}; // UNIT_GUID per equipment slot, 0 if empty

  bool operator==(const PlayerInfo&) const = default;
};

// Snapshot of one player avatar. Grows as reverse engineering exposes more state.
struct PlayerState
{
  uint32_t tick = 0;
  float pos[3] = {};
  float yaw = 0.0f;
  uint32_t levelId = 0; // level name hash mixed with its layout seed; avatars are only
                        // shown to players whose level has the same id
  bool inGame = false;  // false on the title screen / while loading
  bool hasPet = false;
  float petPos[3] = {};
  float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // character rotation, row-major 3x3 of the
                                              // unit transform (+0xAC)
  float health = 0.0f; // +0x394
  uint32_t maxHealth = 0;
  bool dead = false;   // AI state dying/dead
  float petHealth = 0.0f;
  uint32_t petMaxHealth = 0;
  bool hardcore = false; // a hardcore death is final: the party stops waiting for them
};

class Writer
{
public:
  explicit Writer(Msg type) { u8(static_cast<uint8_t>(type)); }

  void u8(uint8_t v) { m_buf.push_back(v); }
  void u16(uint16_t v) { raw(&v, 2); }
  void u32(uint32_t v) { raw(&v, 4); }
  void f32(float v) { raw(&v, 4); }
  void str(const std::string& s)
  {
    uint8_t n = static_cast<uint8_t>(s.size() > 255 ? 255 : s.size());
    u8(n);
    raw(s.data(), n);
  }

  // Appends another message's bytes (type byte included), for relaying.
  void message(const Writer& other)
  {
    u32(static_cast<uint32_t>(other.size()));
    raw(other.data(), other.size());
  }

  void bytes(const std::vector<uint8_t>& b)
  {
    u32(static_cast<uint32_t>(b.size()));
    raw(b.data(), b.size());
  }

  const uint8_t* data() const { return m_buf.data(); }
  size_t size() const { return m_buf.size(); }

private:
  void raw(const void* p, size_t n)
  {
    const auto* b = static_cast<const uint8_t*>(p);
    m_buf.insert(m_buf.end(), b, b + n);
  }
  std::vector<uint8_t> m_buf;
};

// Reads never run past the end; on underflow ok() turns false and values read as 0.
class Reader
{
public:
  Reader(const uint8_t* data, size_t size) : m_p(data), m_end(data + size) {}

  uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
  uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
  uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
  float f32() { float v = 0; raw(&v, 4); return v; }
  std::string str()
  {
    uint8_t n = u8();
    if (!m_ok || static_cast<size_t>(m_end - m_p) < n)
    {
      m_ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(m_p), n);
    m_p += n;
    return s;
  }

  std::vector<uint8_t> bytes()
  {
    uint32_t n = u32();
    if (!m_ok || static_cast<size_t>(m_end - m_p) < n)
    {
      m_ok = false;
      return {};
    }
    std::vector<uint8_t> b(m_p, m_p + n);
    m_p += n;
    return b;
  }

  bool ok() const { return m_ok; }

private:
  void raw(void* out, size_t n)
  {
    if (!m_ok || static_cast<size_t>(m_end - m_p) < n)
    {
      m_ok = false;
      return;
    }
    std::memcpy(out, m_p, n);
    m_p += n;
  }
  const uint8_t* m_p;
  const uint8_t* m_end;
  bool m_ok = true;
};

inline void writeState(Writer& w, const PlayerState& s)
{
  w.u32(s.tick);
  w.f32(s.pos[0]);
  w.f32(s.pos[1]);
  w.f32(s.pos[2]);
  w.f32(s.yaw);
  w.u32(s.levelId);
  w.u8(s.inGame ? 1 : 0);
  for (float f : s.rot)
    w.f32(f);
  w.u8(s.hasPet ? 1 : 0);
  if (s.hasPet)
  {
    w.f32(s.petPos[0]);
    w.f32(s.petPos[1]);
    w.f32(s.petPos[2]);
    w.f32(s.petHealth);
    w.u32(s.petMaxHealth);
  }
  w.f32(s.health);
  w.u32(s.maxHealth);
  w.u8((s.dead ? 1 : 0) | (s.hardcore ? 2 : 0));
}

inline PlayerState readState(Reader& r)
{
  PlayerState s;
  s.tick = r.u32();
  s.pos[0] = r.f32();
  s.pos[1] = r.f32();
  s.pos[2] = r.f32();
  s.yaw = r.f32();
  s.levelId = r.u32();
  s.inGame = r.u8() != 0;
  for (float& f : s.rot)
    f = r.f32();
  s.hasPet = r.u8() != 0;
  if (s.hasPet)
  {
    s.petPos[0] = r.f32();
    s.petPos[1] = r.f32();
    s.petPos[2] = r.f32();
    s.petHealth = r.f32();
    s.petMaxHealth = r.u32();
  }
  s.health = r.f32();
  s.maxHealth = r.u32();
  uint8_t flags = r.u8();
  s.dead = (flags & 1) != 0;
  s.hardcore = (flags & 2) != 0;
  return s;
}

inline void writeInfo(Writer& w, const PlayerInfo& i)
{
  w.str(i.className);
  w.str(i.petType);
  w.str(i.charName);
  w.str(i.petName);
  w.u32(static_cast<uint32_t>(i.petGuid));
  w.u32(static_cast<uint32_t>(i.petGuid >> 32));
  w.u16(i.petLevel);
  uint8_t n = 0;
  for (uint64_t e : i.equipment)
    n += e ? 1 : 0;
  w.u8(n);
  for (uint8_t slot = 0; slot < kEquipSlots; slot++)
  {
    if (!i.equipment[slot])
      continue;
    w.u8(slot);
    w.u32(static_cast<uint32_t>(i.equipment[slot]));
    w.u32(static_cast<uint32_t>(i.equipment[slot] >> 32));
  }
}

inline PlayerInfo readInfo(Reader& r)
{
  PlayerInfo i;
  i.className = r.str();
  i.petType = r.str();
  i.charName = r.str();
  i.petName = r.str();
  i.petGuid = r.u32();
  i.petGuid |= static_cast<uint64_t>(r.u32()) << 32;
  i.petLevel = r.u16();
  uint8_t n = r.u8();
  for (uint8_t k = 0; k < n && r.ok(); k++)
  {
    uint8_t slot = r.u8();
    uint64_t guid = r.u32();
    guid |= static_cast<uint64_t>(r.u32()) << 32;
    if (slot < kEquipSlots)
      i.equipment[slot] = guid;
  }
  return i;
}

} // namespace tlmp::net
