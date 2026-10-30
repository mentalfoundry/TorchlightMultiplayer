TLMP - Torchlight co-op for up to 4 players
Build {VERSION}
=============================================

WHAT YOU NEED
- Torchlight (the first one), version 1.7.8.70 from GOG. Other versions (e.g. Steam's) are refused
  by the installer.
- Everyone in a game must run the same TLMP build. The build is shown on the title screen
  ("Co-op: not in a session (TLMP ...)").
- Back up your saves first, to be safe: copy the folder
  %APPDATA%\Runic Games\Torchlight\save
  (paste that into the Explorer address bar).

INSTALL
1. Close Torchlight.
2. Extract this zip anywhere (or straight into your Torchlight folder).
3. Double-click Install.bat. It finds your Torchlight folder (or asks you to pick it) and may ask
   for administrator rights if the game is under Program Files.

HOST A GAME
1. Start Torchlight and click "Host Co-op" on the title screen.
2. The screen shows "Friends join at <address>". Click "Copy" and send that address to your friends.
3. Click "Continue" to load your character. Friends can join before or after you do.
- If Windows asks whether Torchlight may use the network, click Allow.
- The game asks your router to open UDP port 27960 by itself (UPnP). If friends can't connect and
  the title screen says so, forward UDP port 27960 to your PC in your router's settings. Playing
  over a VPN such as Tailscale or ZeroTier also works: give friends your VPN address instead.

JOIN A GAME
1. Start Torchlight, click "Join Co-op", paste the host's address (Ctrl+V or the Paste button) and
   click "Connect".
2. Click "Continue" to load your own character. You are taken to the host's floor.

PLAYING TOGETHER
- Everyone plays their own character and keeps their own save, gear and quests.
- The party shares one dungeon floor. Taking stairs or a waypoint to another floor waits until
  everyone is at the exit. Trips to town are always allowed alone.
- Chat: press Enter, type, press Enter to send (Esc cancels).
- Your friends' names, levels and health are shown under your pet's portrait (top left), and they
  are blue dots on the map (you are the purple one).
- Like Diablo 2, monsters get tougher with more players: +50% health and +6.25% damage for each
  extra player. Kills give more XP, which is shared between the players near the kill by level
  (players far below or above the monster's level get less).
- Loot: everyone rolls their own drops, and monsters drop something more often when other players
  are near the kill (about +25% per extra player for ordinary monsters). Items a player drops on
  purpose can be picked up by anyone.
- If your connection drops, the game reconnects by itself for up to a minute. If the host leaves,
  everyone else keeps playing on their own.
- The developer console's cheat commands are disabled during co-op.

PROBLEMS
- Logs are in the TLMP_logs folder inside your Torchlight folder (one file per game start). Send the
  newest one along with a description of what happened.
- "The host turned the connection down": the game is full (4 players), or you don't all have the
  same TLMP build.
- Settings (port, monster health per player, ...) are in TLMP.ini in your Torchlight folder.

UNINSTALL
Close Torchlight and double-click Uninstall.bat. The game's own files are put back. Your saves are
not touched.
