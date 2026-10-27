#pragma once

#include "net/protocol.h"

#include <cstdint>
#include <string>

namespace tlmp::net {
class Session;
}

// Who runs the party's dungeon floor.
//
// Each machine loads its own copy of a floor; exactly one of them -- the owner -- runs its monsters
// and doors, and everyone else on that floor shows copies (monsters.cpp, triggers.cpp). The owner
// is the host when the host is on the floor, otherwise the lowest-slot client there; the host's
// process decides and announces it, even while the host itself is in town or on the title screen.
//
// Handover: the old owner sends a snapshot (monsters: id, type, position, health, dead; doors:
// state) and turns its live monsters into copies; the new owner turns its copies (same ids) into
// live monsters and applies the snapshot. When everyone has left a floor the host keeps its last
// snapshot and hands it to whoever arrives next. On a fresh floor the owner is chosen before the
// party travels, so that machine lets the floor spawn its monsters as it loads.
//
// Town has no owner: no hostile monsters live there, and players in town see each other wherever
// the host is.
namespace tlmp::game::floor {

struct PartyFloor
{
  bool known = false;   // the party has a dungeon floor (someone in the game is on one)
  uint32_t levelId = 0;
  std::wstring levelName;
  std::wstring dungeon;
  int floor = 0;
  int owner = -1;       // slot running it, -1 while nobody (handover in progress)
};

void attach(net::Session* session);
void tick();
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);
void onPlayerLeft(uint8_t slot);
void endSession();

// --- this machine's role -------------------------------------------------------------------
// True when this machine runs `level` (or, without a session, always).
bool ownsLevel(const void* level);
bool ownsCurrentLevel();
// Slot that runs this machine's current floor, or -1.
int currentOwner();
// The first unit added to a new level calls this before anything is decided about it.
void onLevelLoading(void* level);
// Party travel sets whether this machine will own the floor it is about to load.
void expectOwnership(bool own);
// Host: a party trip is leaving; `ownerSlot` will run the floor it arrives on.
void onPartyTravel(int ownerSlot);
// The level is being destroyed (before its units are freed).
void onLevelDestroyed(void* level);

// --- party floor, as the host announced it --------------------------------------------------
const PartyFloor& partyFloor();
// Someone else (in the game, not in town) is on a dungeon floor: arrivals join it.
bool partyFloorElsewhere();
bool isTownLevel(uint32_t levelId);
// Each player's whereabouts as this machine knows them.
bool slotInGame(uint8_t slot);
uint32_t slotLevel(uint8_t slot);

// --- routing ------------------------------------------------------------------------------
// To one slot (through the host when needed) / to everyone else on this machine's level.
void sendTo(uint8_t slot, const net::Writer& w, bool reliable);
void sendToFloor(const net::Writer& w, bool reliable);
// Unwraps a Relay; calls `dispatch` for messages meant for this machine.
using DispatchFn = void (*)(uint8_t fromSlot, net::Msg type, net::Reader& r);
void onRelay(uint8_t fromSlot, net::Reader& r, DispatchFn dispatch);

} // namespace tlmp::game::floor
