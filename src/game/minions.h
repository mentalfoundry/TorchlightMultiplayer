#pragma once

#include "game/engine.h"
#include "net/protocol.h"

#include <cstdint>

namespace tlmp::net {
class Session;
}

// Summoned minions across games, kept like pets. Each player reports their minions (type,
// position, health, alive) a few times a second; every other machine shows them with copies that
// belong to that player's stand-in: the minions its replayed summon created (skills.cpp), adopted
// by type, or new ones if none came (joined late, replay refused). Copies walk to the reported
// positions, show the reported health and die when the minion does. Like every stand-in's unit,
// a copy's own hits are for show; on the floor's owner, monster hits on a copy go to the minion's
// player, who applies them to the real minion.
namespace tlmp::game::minions {

void attach(net::Session* session);
void tick();
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);
void onLevelDestroyed(void* level);
void onUnitDestroyed(void* unit); // CCharacter destructor hook

// A copy of a remote player's minion: whose (slot) and which (index in their report).
bool copyOf(const void* unit, uint8_t* slot, uint8_t* index);
// The local player's minion at `index` of our last report (null if gone).
engine::Unit* localMinion(uint8_t index);

// Test mode.
unsigned copiesShown(uint8_t slot);
engine::Unit* anyCopy(uint8_t slot);

} // namespace tlmp::game::minions
