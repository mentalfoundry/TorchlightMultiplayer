#pragma once

#include "net/protocol.h"

#include <cstdint>

namespace tlmp::net {
class Session;
}

// Quests across games.
//
// Quest progress lives in each player's own save; each game only advances its own quests, exactly as
// in single player (which quests a player can start is their own save's business too). What co-op
// changes is who creates the monsters:
//
// A quest places its monsters (DEFEAT targets, quest champions) through CQuestUnitData::Spawn
// (engine.h, "quests"), tagging each with the quest (GUID + quest-unit index). The quest counts a
// kill when the "KILL" event of a unit with its tags reaches it -- which our copies do too: they are
// built from the unit's saved state (tags included) and a client kills its copy as its own kill.
//
// On a floor some other machine runs, a client may not create monsters, so its quest monsters are
// sent to the floor's owner instead: the client lets its game build the monster (tags and all),
// takes it back out of its level and sends the saved state; the owner creates it for everyone --
// unless a living monster with the same quest tags is already there (both players on the same
// quest: one boss, and the kill counts for both).
namespace tlmp::game::quests {

void attach(net::Session* session);
void tick();
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);

// CQuestUnitData::Spawn hook.
using SpawnFn = void*(__thiscall*)(void* questUnit, void* resources, const void* position);
void* spawn(SpawnFn original, void* questUnit, void* resources, const void* position);

} // namespace tlmp::game::quests
