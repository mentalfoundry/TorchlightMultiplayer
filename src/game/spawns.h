#pragma once

#include "net/protocol.h"

#include <cstdint>

namespace tlmp::net {
class Session;
}

// What makes monsters appear, made to work with several players (engine.h, "spawning").
//
// Trigger volumes (CTriggerBox/CTriggerSphere) test every registered player each frame but were
// written for one: with one player inside and another outside they flip every frame and re-fire.
// Outside town, an area's event now happens only when the whole party is together: someone steps
// in and everyone in the game (wherever they are; not the hardcore dead or the title screen) is
// close by. Until then it waits, telling those nearby whom it waits for, and those it waits for
// that the party is waiting.
//
// Formation nodes (the monster groups placed in a floor) wake up for the position the level update
// passes (the local player's). On the machine running the floor they are also offered every other
// player's position. Groups are placed differently on each machine (they don't match by position
// across games), so each machine keeps its own; the snapshot only carries the owner's list.
namespace tlmp::game::spawns {

void attach(net::Session* session);
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);

// Trigger volume update hooks (box / sphere): collect the per-player calls, then apply one.
void beginTriggerUpdate(void* trigger);
// Inside/outside handler hooks: true = run it now (not inside a collected update).
bool onTriggerInside(void* trigger, void* player);
bool onTriggerOutside(void* trigger, void* player);
using HandlerFn = void(__thiscall*)(void* trigger, void* player);
void endTriggerUpdate(void* trigger, HandlerFn inside, HandlerFn outside);

// Formation node update hook: after the game's own call, for the other players on the floor.
using NodeUpdateFn = uint32_t(__thiscall*)(void* node, uint32_t a, uint32_t b, const void* position);
void afterNodeUpdate(void* node, uint32_t a, uint32_t b, NodeUpdateFn original);

// Test mode: the closest monster group on this floor that hasn't spawned yet (at least minDistance away).
bool nearestWaitingGroup(float minDistance, float* x, float* z);
void countGroups(unsigned* total, unsigned* waiting);
void listWaiting(void (*fn)(float x, float z, void* ctx), void* ctx);

// Unit activity check hook (0x48BDF0): on the machine running the floor, a unit far from its own
// player is checked again against every other player there (else monsters near them freeze).
using ActivityFn = void(__thiscall*)(void* unit, const void* position);
void afterActivityCheck(void* unit, ActivityFn original);

// Floor handover (floor.cpp).
void writeSnapshot(net::Writer& w);
void applySnapshot(net::Reader& r);

} // namespace tlmp::game::spawns
