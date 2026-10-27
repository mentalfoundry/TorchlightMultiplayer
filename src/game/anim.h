#pragma once

#include "net/protocol.h"

#include <cstdint>
#include <string>

namespace tlmp::net {
class Session;
}

// Animations across games. The copies on other machines (stand-ins, pet copies, monster copies)
// walk with their own movement code, so idle/run/walk already look right; what they never do on
// their own is attack or cast. Those are one-shot animations (engine.h, "animation"): when one
// starts on the local character, the local pet, or (host) a monster, its name and arguments are
// sent, and the copy plays the same animation, followed by whatever the game queued after it.
namespace tlmp::game::anim {

enum class Call : uint8_t
{
  Play = 0,      // 0x4C8AD0
  Crossfade = 1, // 0x4CA8E0
  Queue = 2,     // 0x4C8950
};

void attach(net::Session* session);
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);

// Animation-call hooks: `animObject` is the hook's this ([unit+0x1AC]), args are the call's raw
// stack arguments after the index.
void onCall(void* animObject, Call call, uint32_t index, const uint32_t* args, int argCount);
bool replaying();

// Test mode: starts the one-shot animation `name` on a unit (as an attack would).
bool playOneShot(void* unit, const char* name);
// Test mode: the last animation of that player (or their pet) replayed on its copy here.
std::string lastReplayed(uint8_t slot);

} // namespace tlmp::game::anim
