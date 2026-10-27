#pragma once

#include "game/engine.h"
#include "net/protocol.h"

#include <cstdint>
#include <vector>

namespace tlmp::net {
class Session;
}

// Host-authoritative monsters.
//
// Host: simulates normally (remote players count as players there, see game.cpp); tracks every
// hostile monster on its level, assigns network ids, and sends a roster (id, UNIT_GUID, spawn
// position), per-client nearby state (position, health, AI state) and deaths.
//
// Client: never creates hostile monsters of its own (AddUnit refuses them). Every hostile
// monster it shows is a puppet created from the host's roster, moved by host state with its
// own AI off, and killed with the game's Kill when the host reports the death.
namespace tlmp::game::monsters {

void attach(net::Session* session);
void tick();
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);
void onLevelDestroyed(void* level);

// Called right before the game saves: our copies must not be written into the floor state
// (they would come back on a revisit as frozen, ownerless monsters).
void beforeSave();

// Hook decisions (main thread).
bool allowAddUnit(void* level, void* unit, uintptr_t caller);
bool shouldSkipAI(void* monster);
bool shouldBlockKill(void* unit);

// Network ids (see combat.cpp). Client: the host's id for one of our copies, and back. Host:
// the id of a tracked monster, and back. 0 / null when unknown.
uint32_t puppetId(const void* unit);
engine::Unit* puppetById(uint32_t id);
bool isPuppetUnit(const void* unit);
uint32_t trackedId(const void* unit);
engine::Unit* trackedById(uint32_t id);
// Host: id of the tracked, living monster whose animation object ([unit+0x1AC]) this is, or 0.
uint32_t trackedIdByAnimation(const void* animObject);

// Floor handover (floor.cpp). The owner writes what it runs; becoming the owner turns our copies
// into the real monsters (same ids) and applies a snapshot if given (anything else hostile on the
// floor then goes); becoming a mirror turns the monsters we ran into copies.
void writeSnapshot(net::Writer& w);
// Test mode: the monsters this machine runs (copy = false) or shows copies of (copy = true).
void forEachKnown(void (*fn)(uint32_t id, engine::Unit* unit, bool copy, bool dead, void* ctx), void* ctx);
void becomeOwner(net::Reader* snapshot);
void becomeMirror();
// We ran a floor someone else runs too (both loaded it at once): our own monsters go, copies of
// theirs come with their roster.
void discardOwn();
// Monster health added per player in the game beyond the first (the host's setting; 0.5 = +50%).
void setHealthPerPlayer(float perPlayer);
// Test mode: the range of party multipliers on the monsters this machine runs (false: none).
bool scaleRange(float* lo, float* hi);
// Copies built from the monster's type alone so far (their look -- names, champions, health -- not
// in yet). Test mode.
unsigned copiesWithoutLook();

// Quests (quests.cpp).
void allowLocalSpawns(bool allow);  // let our own AddUnit calls through on a floor run elsewhere
bool isMonster(const void* unit);
bool captureState(engine::Unit* unit, std::vector<uint8_t>& out); // the game's save state (+max health)
void retireUnit(engine::Level* level, engine::Unit* unit);         // out of the level, out of sight
engine::Unit* spawnFromState(const std::vector<uint8_t>& state, const engine::Vec3& pos);
bool otherLivingWithQuestTag(const void* except, uint64_t questGuid, uint32_t index);

} // namespace tlmp::game::monsters
