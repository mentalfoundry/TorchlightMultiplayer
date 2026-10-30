#pragma once

#include "net/protocol.h"

#include <cstdint>

namespace tlmp::net {
class Session;
}

// Items players drop. Loot from kills, chests and breakables is each player's own (rolled on
// their own machine); an item a player drops from their inventory is shared:
//
// The dropper's game saves the item into the game's own item save state (the shared stash's
// format, engine.h) and sends it; every other game builds the same item from it on that floor.
// Picking a shared item up is the host's call: the first claim wins, the winner's game picks it
// up, everyone else's copy disappears. Copies are taken off the ground before the game saves
// (and put back after), so no floor save ever holds one; dropped items last for the session.
namespace tlmp::game::loot {

void attach(net::Session* session);
void tick();
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);

// CLevel::AddItem hook: before tells whether this is the local player dropping an item, after
// shares it.
bool beforeAddItem(void* level, void* item, uintptr_t caller);
void afterAddItem(void* level, void* item, bool playerDrop);
// CCharacter::PickUp hook: false = not now (shared item: the host is asked first).
bool allowPickUp(void* character, void* item, void* level);

void beforeSave();

// Party loot bonus (PartyRules::lootPerPlayer). While a kill pays out to the local player (its own,
// its pet's or minions'; the Kill hook brackets it), the monster's treasure table drops "nothing"
// less often: a table (CSpawnClass, resolved by 0x53ABB0) weighs its entries, and the UNITTYPE:NONE
// ones are made lighter for that one call, then put back.
void setLootBonus(float perPlayer);
void beginKill(void* victim, int killer);
void endKill();
struct PickWeights
{
  int* weight[8] = {};
  int saved[8] = {};
  int count = 0;
  bool counted = false; // beforePick counted a nesting level (afterPick undoes it)
};
PickWeights beforePick(void* spawnClass);
void afterPick(const PickWeights& w);
// Tests: the last table the bonus changed: its chance of dropping something before and after, and
// the players near that kill.
struct Bonus
{
  float before = 0.0f, after = 0.0f;
  int nearby = 0;
  unsigned count = 0; // tables changed so far
};
Bonus lastBonus();

// Test mode: every shared item this machine knows of (unit null while not on the ground here).
void forEachShared(void (*fn)(uint32_t key, void* unit, void* ctx), void* ctx);
void onLevelDestroyed(void* level);
void endSession();

} // namespace tlmp::game::loot
