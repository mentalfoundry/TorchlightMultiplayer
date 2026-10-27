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

Scenarios live in `tests\scenarios\<name>\host.txt` / `client.txt`. Tests use the profiles `t1` (host) and `t2`
(client) under `<game>\tlmp_profiles`, whose saves are reset from `tlmp_profiles\_test_seed` before every run;
the real profiles are never touched. Two game windows open and close while tests run.

## Status / roadmap

| Milestone | State |
|---|---|
| 0. Mod loader, logging, config, ENet session (handshake, slots, relay), per-instance profiles | **done, tested with 2 local instances** |
| 1a. Read local player position/orientation | **done** (`CGame[0xC1AD64]+0x64 → +0x2C → +0x58 node`) |
| 1b. Remote avatars + pets: spawned via the game's factories, walked with click-to-move, survive level changes | **done, tested in town and dungeon** |
| 1c. Polish: character/pet names, equipment appearance (by item GUID), idle facing, chat | **done; offline-tested (mirror mode), needs in-game test** |
| 2. Shared level: clients adopt the host's world seed so floors generate identically (own seed kept in saves) | **done, user-verified in dungeon** |
| 3. Host-authoritative world: host spawns everything (remote players registered as players there); clients refuse their own hostile spawns and show puppets of the host's monsters; doors/levers/portals are requests the host performs; party travel (map changes wait until everyone is at the exit; clients join the host's floor after Continue) | **built, smoke-tested; needs in-game test** |
| 4. Combat & skills: client input → host simulation → replicated effects | |
| 5. Loot, gold, items, quests, waypoints, town portals | |

Reverse-engineering notes and tools are in [re/](re/) (`NOTES.md` has every verified address).
