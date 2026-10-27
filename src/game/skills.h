#pragma once

#include "game/engine.h"
#include "net/protocol.h"

#include <cstdint>
#include <string>

namespace tlmp::net {
class Session;
}

// Skills across games. A player's skills run on their own machine as in single player: the hits
// they cause go through combat (TakeDamage) like any other hit. What other machines need is to SEE
// the skill: so when the local player starts one (CCharacter::UseSkill, engine.h "skills"), the
// skill, its level and where it was aimed go to everyone on the floor, and each machine has that
// player's stand-in cast it too -- learning it first if needed (stand-ins are fresh characters).
//
// A stand-in's cast is for show: whatever damage it or its summons would do is dropped (combat.cpp),
// since the real player's hits already arrive from their machine. Everything else a skill does
// happens as in the game (missiles, effects, summoned minions on every machine).
namespace tlmp::game::skills {

void attach(net::Session* session);
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);

// CCharacter::UseSkill hook.
uint32_t useSkill(engine::UseSkillFn original, engine::Unit* unit, uint32_t guidLo, uint32_t guidHi);

// Units whose damage is for show: stand-ins and the minions they summon.
bool isShowOnly(const void* unit);

// Test mode.
bool learnForTest(engine::Unit* unit, const std::string& name); // learn it and put it on the right button
unsigned castsSent();
unsigned castsPlayed();

} // namespace tlmp::game::skills
