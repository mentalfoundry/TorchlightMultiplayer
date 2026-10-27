#pragma once

#include "game/engine.h"
#include "net/protocol.h"

#include <cstdint>

namespace tlmp::net {
class Session;
}

// Combat across games. Every hit in the game ends in CCharacter::TakeDamage (engine.h), so that
// is where hits cross over:
//
// Client hits a monster: the hit is computed locally (the client's real stats, skills and gear)
// and lands on its copy for the feel of it (reaction, knockback), but never takes it to 0 health.
// The same TakeDamage arguments go to the host, which applies them to the real monster with the
// player's stand-in as the attacker: aggro, knockback and the kill happen there, and come back
// through the monster sync.
//
// Host monster hits a stand-in (or a remote pet's copy): nothing happens to the stand-in; the
// hit goes to its owner, who applies it to the real character or pet.
//
// Kills: whoever is near (kShareRadius) gets the XP and fame. The host gets them from the game
// as usual, or redirected from the stand-in that made the kill; clients get them when the host
// reports the death, never from their own copies.
namespace tlmp::game::combat {

// CCharacter::TakeDamage's arguments after the target.
struct Hit
{
  engine::Level* level = nullptr;
  const engine::Vec3* sourcePos = nullptr;
  float damage = 0.0f;
  const engine::Vec3* dir = nullptr;
  float force = 0.0f;
  engine::Unit* attacker = nullptr;
  char showNumber = 0;
  char react = 0;
  char knockback = 0;
  int arg11 = 0;
  uint32_t flags = 0;
};

void attach(net::Session* session);
void tick();
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);

// TakeDamage hook: false drops the hit here. May lower hit.damage.
bool beforeTakeDamage(engine::Unit* target, Hit& hit);
// GiveExperience / GiveFame hook: false = not given to `unit` (it may have gone to someone else).
bool allowReward(engine::Unit* unit, engine::Level* level, engine::Unit* victim, int amount, char raw, bool fame);
// Client: the host reports that the monster this copy stands for died.
// Returns true when this player shares in the kill (close enough): the copy is then killed with the
// local player as the killer, so the game drops this player's own loot and counts it for their quests.
bool onHostKill(engine::Unit* puppet);
// Kill hook: the killer to pass on (a stand-in's kill becomes the local player's share, or nobody's).
int killerFor(engine::Unit* victim, int killer);

} // namespace tlmp::game::combat
