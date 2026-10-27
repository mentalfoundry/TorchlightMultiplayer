#pragma once

// Helpers shared between the game-side modules (avatars in game.cpp, monsters.cpp, ...).
// All of these run on the game's main thread.

#include "game/engine.h"

#include <cstdint>
#include <string>

namespace tlmp::game {

using engine::Vec3;

// Seconds since TLMP started ticking.
float clock();

uintptr_t gameClient();
uintptr_t localPlayer();
engine::Level* currentLevel();
// Level name mixed with its layout seed; equal ids mean the same floor built the same way.
uint32_t levelId(engine::Level* level);
Vec3 unitPosition(engine::Unit* unit);
float distXZ(const Vec3& a, const Vec3& b);

// A unit driven from network state: walked with the game's click-to-move (so it paths and
// animates), snapped when too far behind.
constexpr float kTeleportDistance = 12.0f; // beyond this, snap instead of walking
constexpr float kArriveDistance = 0.4f;
constexpr float kReorderDistance = 0.3f;   // re-issue MoveTo when the target moved this far
constexpr float kOrderInterval = 0.1f;     // MoveTo runs A*, so rate-limit it

struct Puppet
{
  engine::Unit* unit = nullptr;
  engine::Level* level = nullptr; // level the unit was added to; null once that level is gone
  Vec3 lastOrder{};
  float lastOrderTime = -1.0f;
};

// Moves p toward target; returns true when it is standing at the target.
bool steer(Puppet& p, const Vec3& target);

std::string toUtf8(const wchar_t* w);
std::wstring fromUtf8(const std::string& s);

// HUD notice (the small message line), held for `seconds`.
void showNotice(const std::string& utf8, float seconds);

// Shows a remote unit's health on its copy (health bar, what monsters see): max and current.
void applyVitals(engine::Unit* unit, float health, uint32_t maxHealth);

// True for the units we spawned to represent remote players (character or pet).
bool isAvatarUnit(const void* unit);
// True while we are building a remote player's stand-in (CreatePlayer up to its placement).
bool creatingStandIn();

// Remote player's avatar character / pet copy on this machine, or null.
engine::Unit* avatarOf(uint8_t slot);
engine::Unit* avatarPetOf(uint8_t slot);
// Slot whose avatar or pet copy `unit` is (isPet tells which), or -1.
int avatarSlotOf(const void* unit, bool* isPet);

// The local character's pet (client+0x30, checked to be a CMonster), or 0.
uintptr_t localPet();

// Death screen and tutorial tips belong to the local player only (hooks, see engine.h).
void setUpdatingPlayer(void* player);
bool allowPlayerUi();
// True while we kill a stand-in because its player died.
bool killingStandIn();

// True for the stand-in game clients the host registers for remote players.
bool isRegisteredClient(const void* client);

} // namespace tlmp::game
