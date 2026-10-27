#pragma once

#include "net/protocol.h"

#include <cstdint>
#include <string>

namespace tlmp::net {
class Session;
}

// Test mode (-tlmp-test <file>): the game plays a scenario by itself and records what happened, so
// co-op can be checked without anyone at the keyboard (tools/test.ps1 runs host and client together).
//
// A scenario is one command per line ('#' starts a comment); the host and each client run their own.
// Commands drive the game through its own functions (walk = click-to-move, hits end in TakeDamage,
// buttons are clicked through CEGUI), `sync` lines meet the other machines, `expect` lines check
// state (retrying until true or a timeout), and `dump`/`shot` record state and screenshots for
// comparing machines afterwards. Results go to <game>\tlmp_test\<profile>\.
//
//   log <text>                 wait <seconds>            players <n>           sync <name>
//   click <CEGUI window>       ingame                    mark                  quit
//   walkto <x> <z> | walkto player <slot> | walkto monster
//   hit <damage>               killnear                  anim <name>           die
//   drop                       pickup                    townportal            stairs up|down
//   expect avatar|noavatar|pet <slot>    expect owner <slot>      expect monsters <min>
//   expect xpgain              expect notice <text>      expect items <delta>  expect shared <n>
//   expect hurt (health below the last mark)   expect noshared   expect newlevel (since mark)
//   expect avatardead <slot>      expect anim <slot> <name>   expect moremonsters (than at mark)   expect groundloot (new items near me since mark)
//   expect lootrolls <n> (our game rolled treasure for n kills since mark; what drops is random)
//   teleport area [min distance]  (into the nearest trigger area not fired yet)   expect area waiting|fired|unfired
//   expect quest <name> [state]   expect questdone <name>   killquest [approach]   key shift down|up
//   share node [min]  (send our nearest unspawned monster group's position)   teleport shared
//   hardcore on|off  (this character, in memory)   skill right|slot <n>  (use it like player input)
//   expect looks  (every monster copy here has its owner's look)
//   expect monsterscale <x> | copyscale <x>  (party health multiplier on the monsters run / copies here)
//   expect minioncopies <slot> <n> | nominioncopies <slot>  (copies of that player's minions here)
//   expect minionhurt (ours)   hitminioncopy <slot> [damage]   killminion  (test only)
//   expect castsent <n> | castplayed <n>  (our casts sent / other players' casts shown here, since mark)
//   walkto node [min distance]  (the nearest monster group not spawned yet)   teleport node [min]   teleport monster <min distance from the host>   teleport player <slot>
//   expect dead|alive          expect town|dungeon       expect samelevel <slot>
//   dump <tag>                 shot <tag>                console <command line> (cheats allowed)
// Any command takes "timeout=<seconds>" at the end.
namespace tlmp::game::autotest {

void start(net::Session* session, const std::wstring& scriptPath);
void tick();
bool active();
void onNotice(const std::string& text);
void onMessage(uint8_t fromSlot, net::Msg type, net::Reader& r);

} // namespace tlmp::game::autotest
