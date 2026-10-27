#pragma once

#include "net/protocol.h"

#include <cstdint>

namespace tlmp::net {
class Session;
}

// Doors, levers, gates and other openables (CTriggerUnit). Floors are generated from the
// host's seed, so every machine has the same triggers in the same places; they are matched by
// UNIT_GUID and position. When the local player uses one, the new state is sent (the host
// applies it and relays it); receivers replay the game's own Activate until they reach that
// state, so animations, collision and linked logic all happen normally. The host also sends
// the state of every trigger that has moved, so late arrivals catch up.
namespace tlmp::game::triggers {

void attach(net::Session* session);
void tick();
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);

// True while we are replaying another player's trigger use (such replays must never start a
// map change on this machine).
bool isReplaying();

// CTriggerUnit::Activate hook: on a client the local player's uses become requests to the
// host (the game's own requirement checks have already passed in Interact). True = run now.
bool allowActivate(void* trigger, void* user);

// CTriggerUnit init / destructor hooks: keep the registry of live triggers.
void onTriggerCreated(void* trigger);
void onTriggerDestroyed(void* trigger);

// CTriggerUnit::Interact hook brackets (main thread).
int beforeInteract(void* trigger, void* user);
void afterInteract(void* trigger, void* user, int stateBefore);

// Test mode: the door/lever/chest farthest from the local player.
bool farthestTrigger(float* x, float* z);

// Floor handover (floor.cpp): the states of every used door/lever on the current floor.
void writeSnapshot(net::Writer& w);
void applySnapshot(net::Reader& r);

} // namespace tlmp::game::triggers
