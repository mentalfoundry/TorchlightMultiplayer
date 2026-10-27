# TLMP — Torchlight multiplayer mod

Adds host-authoritative co-op (up to 4 players) to Torchlight 1, **GOG build 1.7.8.70 only**:
the hooks rely on fixed addresses, and other builds are refused at startup.

## Build & install

```powershell
.\build.ps1                 # 32-bit, static CRT; needs VS 2026 + vcpkg (minhook, enet: x86-windows-static)
.\install.ps1               # into D:\GAMES\Torchlight (use -GameDir to change)
.\install.ps1 -Uninstall    # restores Plugins.cfg and the original OgreGUIRenderer.dll
```

Install does three things:
- copies `TLMP.dll` into the game folder
- swaps `OgreGUIRenderer.dll` for a forwarding proxy (the original is kept as `OgreGUIRenderer_orig.dll`)
- adds `Plugin=TLMP` to `Plugins.cfg`

## Running

Set `TLMP.ini` in the game folder, or use these flags:

```
Torchlight.exe -tlmp-host [port]                          # host (slot 0), default UDP 27960
Torchlight.exe -tlmp-join <ip>[:port] -tlmp-name <name>   # client
Torchlight.exe -tlmp-profile <name> ...                   # separate settings/saves (needed for >1 local instance)
Torchlight.exe -tlmp-worldseed <n>                        # testing: generate floors as if a host's world seed were n
```

Chat: open the console with **Shift + `** (the ~ key) and type `/message` or `say message`.
During a session the console is switched on in memory only (your settings files keep their own values), and
its cheat commands are refused; only HELP, CLS, FPS, ROOM, SHOWUI, HIDEUI and ACHIEVEMENTS work.
Chat and names are UTF-8. The HUD fonts can show Latin-1 (e.g. é, ü, ñ); the console font also shows Greek and
Cyrillic. No shipped font has Chinese/Japanese/Korean glyphs.

Logs: `TLMP_<pid>.log` in the game folder (crashes are logged there too).

Testing aids: `-tlmp-mirror` shows your own character as a remote player 2 units away.

## Automated co-op tests

The mod has a test mode (`-tlmp-test <scenario file>`, see `src/game/autotest.h` for the commands) in which the
game plays a scripted scenario by itself: it presses Continue through the real UI, walks, fights, drops and picks
up items, travels, dies and resurrects, meets the other machines at `sync` points, checks state (`expect`), and
writes dumps and screenshots to `<game>\tlmp_test\<profile>\`.

```
.\tools\test.ps1 combat          # one scenario: host + 1 client on this machine, results in tests\runs\
.\tools\test-all.ps1             # every scenario, plus the dump comparisons
python tools\compare.py <run> <tag>   # owner's monsters vs the copies, stand-ins vs the real players
```

Scenarios live in `tests\scenarios\<name>\host.txt` / `client.txt` (or `client1.txt` .. `client3.txt` for more
players). Tests use the profiles `t1` (host) and `t2`..`t4` (clients) under `<game>\tlmp_profiles`, whose saves
are reset from `tlmp_profiles\_test_seed` before every run; the real profiles are never touched. Game windows
open and close while tests run; avoid typing during a run (the game reads some keys from any window).

## How co-op plays

- **Floors.** Everyone shares one dungeon floor, generated identically from the host's world seed (your
  own seed stays in your save). Leaving a floor waits until every player in the game is at the exit,
  then counts down; a softcore player who is dead must resurrect first. Hardcore-dead players and those
  on the title screen don't count (they stay connected and can come back with another character).
- **Town** trips are always allowed alone; everyone in town is in the same town. Going back down puts
  you on the party's floor.
- **Who runs a floor.** The host's machine simulates the floor it is on; if the host is in town or on
  the title screen, the lowest-numbered player on the floor takes over (monsters, doors and events are
  handed over), so the dungeon keeps running.
- **Events.** Scripted trigger areas outside town fire only once everyone in the game is together
  (within about 15 units), with "waiting for ..." messages on screen.
- **Combat.** Hits go to the machine running the floor; XP and fame are shared within 50 units.
  Monsters, champions included, are exact copies on every machine (names, health, positions).
- **Loot.** Each player rolls their own loot for every kill they share in (often nothing, as in single
  player). Items a player drops are shared: whoever picks one up first gets it.
- **Skills.** When you use a skill, everyone else's game has your character cast it too, aimed the
  same way: missiles, effects and summoned minions show up for everyone. Those casts are for show;
  damage only comes from your own game. Your minions are kept like your pet: everyone sees them
  where they are, with their health, and monsters that hit them hurt your real minions.
- **Death.** Each player gets their own death screen.
- **Quests** stay in each player's own save: anyone can take any quest their save allows, whatever floor
  the session is on. A quest's monsters are created by the machine running the floor for everyone; one
  target per quest, and one kill counts for every player holding that quest nearby.

## Status / roadmap

| Area | State |
|---|---|
| Loader, logging, config, ENet session (host relays), per-instance profiles | done |
| Remote players and pets (names, gear appearance), chat | done, user-verified |
| Shared floors (host world seed), party travel, town rules, floor handover | done, rig-tested |
| Monsters run by the floor's owner, exact copies (incl. champions), doors/levers | done, rig-tested |
| Combat, shared XP/fame, health bars, animations, per-player death screens | done, rig-tested |
| Loot: per-player kill loot, shared player drops | done, rig-tested |
| Quests: per-save quests, shared targets and kill credit | done, rig-tested |
| Party-wide trigger events with waiting messages | done, rig-tested |
| Monster groups waking near remote players, quest targets for late arrivals, hardcore deaths, 4 players | done, rig-tested |
| Skills and spells on other machines (cast for show by the player's stand-in), summons | done, rig-tested |
| Summoned minions in sync (copies follow the real ones; hits on copies reach the real minion) | done, rig-tested |
| Party difficulty scaling | not started |
| Stand-in stats (remote players are level-1 characters wearing the real gear) | not started |
| Town portals and waypoints for other players | not started |
| Internet play (lobby/launcher, NAT, reconnects, host leaving) | not started |

Reverse-engineering notes and tools are in [re/](re/) (`NOTES.md` has every verified address).
