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

// Test mode: every shared item this machine knows of (unit null while not on the ground here).
void forEachShared(void (*fn)(uint32_t key, void* unit, void* ctx), void* ctx);
void onLevelDestroyed(void* level);
void endSession();

} // namespace tlmp::game::loot
