#pragma once

#include "net/protocol.h"

#include <cstdint>
#include <string>

namespace tlmp::net {
class Session;
}

// Party travel. Everyone stays on the host's floor; any map change (stairs, waypoints,
// portals) is only a request until the whole party is gathered where it was made. The host
// collects requests, shows everyone how many players are at the exit, and when all are there
// tells every machine to run the same CGameClient::ChangeLevel call, so the party arrives
// together at the same place. Nobody is moved without having walked to the exit.
namespace tlmp::game::party {

struct TravelArgs
{
  std::wstring dungeon;
  int mode = 0;
  int floor = 0;
  char waypoint = 0;
  std::wstring warpName;
  char flag = 0;
};

void attach(net::Session* session);
void tick();
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);

// Bracket the game's "enter the game" routine (0x418BC0): it may move a freshly loaded
// character (e.g. to Town) through ChangeLevel, which must never be held for the party.
// True once the player has pressed Continue (left the title screen) in this run.
bool enteredFromContinue();
void beginEnterGame();
void endEnterGame(bool fromContinue);

// Host: while applying a remote player's trigger use (e.g. their town portal), map changes
// it causes are that player's request. -1 = none.
void setRemoteRequester(int slot);

// CGameClient::ChangeLevel hook: true lets the call through now.
bool allowChangeLevel(void* client, const TravelArgs& args);

} // namespace tlmp::game::party
