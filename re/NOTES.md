# Torchlight (GOG) — reverse engineering notes

Target: `D:\GAMES\Torchlight\Torchlight.exe`, build 1.7.8.70 (BuildVer.txt `1_7_8_70_20100830172 4`),
PE timestamp 1341941781. GOG build, no DRM.

## Binary facts
- x86 (32-bit), MSVC 2008 (msvcr90/msvcp90), ImageBase 0x400000, **no ASLR** (DllCharacteristics 0x8100) → fixed addresses.
- Sections: .text 0x401000 (0x6833cb), .rdata 0xA85000, .data 0xC12000 (0x352c44 virt), .rsrc.
- Engine: Ogre3D (OgreMain.dll, 8192 exports, C++ mangled), CEGUI 0.6-era, FMOD Ex, ParticleUniverse.
- **No networking imports** (no ws2_32/wsock32/wininet). All netcode must be added.
- Imports CreateMutexW / WaitForSingleObject / CreateProcessW — possible single-instance guard (unverified).
- RTTI present: 348 game classes (see `rtti_classes.txt`, `vtables.json`).

## Injection vector
Two stages:
1. `OgreGUIRenderer.dll` forwarding proxy (original renamed `OgreGUIRenderer_orig.dll`) statically imports
   TLMP.dll → TLMP's DllMain runs at process start (used for `-tlmp-profile` AppData redirection via IAT slot
   0xA86398). A CreateRemoteThread launcher was tried and **Windows Defender quarantines it** — don't.
2. Ogre loads every `Plugin=` in `Plugins.cfg` and calls exported `dllStartPlugin()`.
   `Plugin=TLMP` → same module, starts net + frame listener. Resolve Ogre symbols by GetProcAddress (do NOT link
against Ogre headers with a modern CRT — MSVC2008 std::string/STL ABI differs).
- `?getSingletonPtr@Root@Ogre@@SAPAV12@XZ`
- `?addFrameListener@Root@Ogre@@QAEXPAVFrameListener@2@@Z`

## Class map (vtable VA for primary base; sizes where known)
| Class | primary vtable | ctor | dtor | notes |
|---|---|---|---|---|
| CGame | 0xA8F5FC (17) | contains 0x40A8CF | 0x409550 | FrameListener, WindowEventListener, RenderTargetListener |
| CGameClient | 0xA9218C | contains 0x4125B2? | 0x40F970? | "Creating Game Client." log string @0x68DAD8 (unverified which is ctor) |
| CGameGlobals | 0xAAE050 (6) | contains 0x5261EF | 0x524DF0 | getter? `0x5263C0: mov eax,[0xE37904]` |
| CBaseUnit | 0xAA441C (79) | | | ← CPositionableObject ← CSceneNodeObject ← CEditorBaseObject ← CRunicCore |
| CCharacter | 0xAA5E54 (120) | | | + iInventoryListener (@+400), iMissile (@+404) |
| CPlayer | 0xAA8F24 (120) | 0x4DCBA0 | contains 0x4DDEBF | **sizeof 0xA10**, ← CCharacter |
| CMonster | 0xAA883C (120) | | | ← CCharacter |
| CItem | 0xAA82F4 (94) | | | ← CBaseUnit |
| CMissile | 0xABA4AC (58) | | | ← CPositionableObject |
| CLevel | 0xAA9EAC | | | refs 0x4F91AF, 0x4FB744 |
| CConsole | 0xAB2CD8 | | | refs 0x56BDDA, 0x5722AD |

## Object graph — VERIFIED at runtime (RTTI-checked, 2026-10-19)
- `[0x00C1AD64]` = **CGame\*** (only .data slot holding it)
- CGame+0x64 = CGameClient\* (0x402100: `new(0x2484)`, ctor 0x40F970)
- CGameClient: +0x10 CCameraControl, +0x24 CSoundManager, +0x28 CSettings, **+0x2C CPlayer (local)**,
  **+0x30 CMonster (pet)**, +0x34 CQuestManager, **+0x38 CLevel**, +0x3C CGameUI
- CPlayer (0xA10 bytes): **+0x58 Ogre OctreeNode\*** (position via `Node::_getDerivedPosition`),
  +0x60 CResourceManager, +0x178 CDataGroup, +0x17C CEffectManager, +0x184 CSkillManager,
  +0x1AC/+0x1B0 CGenericModel, +0x20C CAstarPathfinder, +0x210 CPath, +0x2CC CAttackDescription,
  +0x2D0 CSkill, +0x404 CInventory, +0x408 CEquipment, +0x43C CCollisionModel, +0x454 CWardrobe,
  +0x660 CAIManager, +0x790 CPath, +0x7B4 CQuestManager
- A CPlayer exists even on the main menu (title-screen character).
- Unit nodes are `OctreeNode` (vtable from Plugin_OctreeSceneManager.dll), not plain SceneNode.

## Singleton getters (getter fn → global → class, runtime-verified)
0x5C75A0→[0xEE2850] CFileSystem · 0x524D40→[0xE373D0] CGameGlobals · 0x5263C0→[0xE37904] CGraphManager ·
0x5EFA20→[0xF25320] CQuestManager · 0x5F9F90→[0xF44EF0] CAchievements · 0x5FC340→[0xF48CB0] CSteamStats ·
0x53F170→[0xE555FC] CGameUI · 0x530FF0→[0xE44500] CUnitResourceList · 0x52DFE0→[0xE40900] CSharedStash ·
0x535C20→[0xE44798] CUnitThemes · 0x5248E0→[0xE34518] CDungeonManager · 0x524490→[0xE30F38] CCinematics ·
0x53C080→[0xE4C020] CSpawnClassParser · 0x52D3C0→[0xE3D320] CSets · 0x5BD100→[0xEDE698] CGameSpeed
Also: [0xF09048] CCameraControl, [0xE37968] CMasterResourceManager, [0xEFB888] CSkillParser, [0xE444E0] CStringTranslate.

## Key functions (decompiled)
- 0x5FE5D0 `UnitFactory::CreatePlayer(this, const wchar_t* className, arg)`: looks up className in the
  "PLAYERS" table (0x530FF0 → 0x532450), new(0xA10), ctor 0x4DCBA0, then vtable[+0xF4](descriptor, arg).
- 0x40CCA0 `CGameClient::SetPlayerClass(wstring)`: deletes old [+0x2C], creates via factory
  `[[client+0x38]+0xD8]` (factory hangs off CLevel+0xD8), then 0x4857E0, 0x4D8770, 0x40C630, 0x4F57D0, 0x4862B0.
- 0x415A50 CGameClient load-character path (reads save file when +0x2C is null).
- 0x5CC6C0 GetUserDataPath: SHGetSpecialFolderPathW(CSIDL_APPDATA) + "/Runic Games/Torchlight/".
- 0x5CCA70 CreateUserDir(relPath): builds from the **APPDATA env var** (0x5CCC1A), not the shell API.
  WinMain uses it for "Runic Games\Torchlight\{Save,Mods}". Profile redirection must override both.
- Saves: "Saving character to :<path>/save/save.tmp" (logged in Ogre.log), written on zone change / quit.
- 0x5FD540 settings load (settings.txt, local_settings.txt) — runs in WinMain *before* Ogre plugins load.
- 0x5FD6B0 "TORCHLIGHT-RESTART" mutex — restart helper, not a single-instance guard.

## Unit / level API used for remote avatars (see src/game/engine.h)
- 0x4857E0 `Unit::SetAlignment(int)` → +0x28C, picks outline texture (player passes 1).
- 0x4F57D0 `CLevel::AddUnit(unit, const Vector3* pos, bool)` — SetPlayerClass passes &level+0xDC (spawn pos), 0.
- 0x4862B0 `Unit::FaceDirection(const Vector3* dir)` — atan2(x,z); SetPlayerClass passes &level+0x100.
- 0x4F7160 `CLevel::RemoveUnit(unit, bool stopActions)` — unlinks from lists +0x5C/+0x64/+0x80, no free.
- 0x494B30 `Character::MoveTo(CLevel*, float x, float z)` — click-to-move; A* via +0x20C into path +0x210;
  target stored at +0x1CC, moving flag +0x208. Called from CGameClient input as (client+0x2C, client+0x38, x, z).
- vtable[10] (+0x28) `SetPosition(x,y,z)` → 0x510F10 SetPosition(const Vector3&); 0x510EB0 GetPosition(out, bool).
- Unit fields: +0x70 position, +0x88/+0x94/+0xA0 orientation axes, +0xAC transform matrix.
- 0x4F9180 CLevel dtor: **deletes every unit in owned list +0x64** (other lists just freed).
  → avatars must be forgotten before it runs (hooked).
- 0x4FB700 CLevel ctor. 0x4D8770 gives starter potions/scrolls. 0x54CBB0 GameUI::SetPlayer.
- 0x40CDE0 `CGameClient::SetPetType(wstring)`: RemoveUnit+delete old [+0x30], `CreateMonster(petType,0,0)`,
  set name from GameUI+0x1418 (0x40C630), SetAlignment(1), **[pet+0x65C]=2**, AddUnit(&level+0x10C), FaceDirection(&level+0x118).
  It does NOT add the pet to the player's minion list — a pet made only this way is saved into the
  floor's units, not the character (learned the hard way: dog record ended up at 0x3E279 in 0.svt).
- 0x4ABD60 `CPlayer::AddMinion(minion)` ret 4: no-op if already listed (0x490760 = index or -1); retires
  excess same-type summons (counting only minions with +0x50E == 0); appends via 0x501A30.
  Pets get **+0x50E = 1** (INVINCIBLE, from 0x49F700) first. Callers: load 0x418E70/0x41970C,
  console FIXPET 0x56D62D (CreateMonster, SetAlignment, AddUnit, 0x50E=1, mode 2, name, AddMinion).
- 0x5FE830 `UnitFactory::CreateMonster(this, name, int level, int arg)`: "MONSTERS" table, new(0x728) CMonster
  (ctor 0x4D5C60), vtable[+0xF4] init. level<1 → factory+0xC level's +0x144.
- Remote pets spawned this way follow MoveTo cleanly (user-verified 2026-10-19); their own AI didn't fight it in town.
- client+0x190 pet type is NOT reliable: seen "Cat" at the title, then empty after the floor change on join
  (2026-10-20). TLMP identifies pets by the unit's UNIT_GUID (+0x168) and builds copies with createMonsterByGuid.
- CLevel+0x1F0 level name wstring ("TOWN"). CPlayer+0x30 class name. client+0x174 class, +0x190 pet type,
  +0x158 save path. client+0xF80: 0 on title screen (transiently 3 while the title character is built).
- Title screen = the TOWN level with the last-played character standing in it.

## Level generation & RNG (milestone 2)
- 0x5BFD70 `SeedRng(DWORD)` cdecl: global state [0xEDE788] (+counter [0xEDE78C]); seed 0 → GetTickCount().
- 0x4FF120 level build. At 0x4FF6DC (ret 0x4FF6E1, ESI = CLevel*):
  `seed = level[+0x140] + level[+0x194] + ([client+0x242C] ? CDungeon+0x38 : 0)`. Logged as "Seeding: ".
  Derived reseeds: 0x4FF85E (same value), 0x50FCB1 (seed+3 … seed+16, per chunk), 0x40D250/0x40EF1D.
- 0x40D250 `CGameClient::SeedRng(floorArg)`: client[+0xFB0] + (arg==-1 ? client[+0xFB4] : arg) + CDungeon+0x38.
- **client+0xFB0 = per-character world seed** (from save; read by load 0x415A50, written to save by
  0x417380 SaveCharacter). It is the ONLY per-player input: level+0x194 is a copy of it; CDungeon+0x38 and
  level+0x140 matched across two different characters.
- level+0x194 can also be forced by a settings entry (CSettings vector via [0xE37968]+0x48, index [0xF60DC0]),
  a dev "fixed seed" option; 0 = unused.
- Determinism verified 2026-10-19 on the title backdrop level "MAINMENUMINE": same seed → identical scene
  fingerprint across runs; switching seeds changes both characters' static scene by the same +21 nodes.
- Title screen is a backdrop level: "MAINMENUMINE" (dungeon) or TOWN depending on where the save is.
- CDungeon (client+0x242C): +0x3C name "MAIN", +0x84 "Town". Level descriptor = level+0x15C (+0x58 name).
- 0x5FE5D0: allocates 0xA10 bytes, constructs CPlayer (0x4DCBA0), then calls vtable[+0xF4] (init(descriptor, ...)).
- 0x5FE4F0: looks up descriptor (0x530FF0 → 0x5321A0), creates unit via 0x5FE210, sets byte [unit+0x1AE],
  calls vtable[+0xF4], then dynamic_cast (0x6058AA = __RTDynamicCast) and 0x4B6830(1).
- Virtual +0xF4 (index 61) on units = init-from-descriptor (hypothesis).

## Tooling (in this folder)
- `rtti_vtables.py` → vtables.json; `xref.py <hex>` immediate refs; `callers.py`+`who.py <hex>` call xrefs;
  `tldis.py <va> [n]` disassemble with string annotation; `strs.py` → strings.txt.

## Combat (2026-10-20)
- 0x4A2380 attacker-side hit (this = attacker, p3 = target, p4 = weapon): "Combat Rolls for Character"
  console trace, weapon roll, crits, armor, leech (AdjustHealth on the attacker), then TakeDamage.
- 0x499750 `CCharacter::TakeDamage(level, const Vec3* src, float dmg, const Vec3* dir, float force,
  CCharacter* attacker, char showNumber, char react, char knockback, int, uint flags)` ret 0x2C — the
  target-side funnel (callers 0x4A44FA weapon, 0x50DE1B missile, 0x5E23C4 effect (flags 0x200), 0x4AB8FA,
  0x499DCD reflect). Early-outs: dmg 0, AI state 5/6, +0x50F == 0. AdjustHealth(-dmg) (0x492860);
  at ≤0: vtable+0x168 Kill(attacker, ..), attacker->GiveExperience(level, target, [target+0x3D4], 0),
  GiveFame(.., [+0x3D8], 0), both zeroed. react: 0x4F1D60(level; attacker, target) + target vt+0x178.
  Reflect back to attacker unless flags & 0x300. Missiles/effects may pass attacker = null.
- 0x492860 `AdjustHealth(float delta)` (+0x394), clamps to max (0x4901E0), INVINCIBLE (+0x50E) floors at 1.
- 0x490420 `GiveExperience(level, victim, amount, raw)` ret 0x10: +bonus unless raw, raw share to every
  minion (+0x5C4), forwards to owner (+0x5B4) unless raw, adds to +0x3C8, level up vt+0x160.
  0x4880E0 `GiveFame` same shape (+0x3CC).

## Animations, health, items (2026-10-20)
- Max health 0x4901E0 = (+0x398 + +0x39C) * (1 + stat 0x14 %) + stat 5. Max mana 0x490330 (+0x3B8 current).
- Unit animation object at unit+0x1AC (thiscall this of the anim calls). Animation set at +0x1D4:
  count +0x28, std::string (char) names 0x1C each from +0x38 (end +0x3C), compared upper-cased
  (0x5D1840); 0x4C3CC0 name -> index. Calls (index first, all 4-byte args):
  0x4C8AD0 play(idx, loop, rate, dur) ret 0x10; 0x4CA8E0 crossfade(idx, loop, blend, rate, dur) ret 0x14;
  0x4C8950 queue(idx, loop, blend, rate) ret 0x10. Unit wrappers: 0x486070 crossfade (sets +0x1FC = 1),
  0x486A80 play-by-name then queue IDLE. Attack 0x496EB0: crossfade swing (loop 0), queue follow-up/IDLE.
- CActiveAnimation vtable 0xAA7EDC, ctor 0x4C1C20(idx, length, loop, rate) (new 0x34).
- Items: CLevel::AddItem 0x4F5960(item, pos, flag) ret 0xC (ground list +0x68); RemoveItem 0x4F7260(item,
  hide) ret 8; CCharacter::PickUp 0x498A20(item, level) ret 8. CGameUI+0x60 = item on the cursor (0x54D830
  returns it to the inventory or drops it at the player). Units vs items: 0x4F70E0 picks AddUnit/AddItem.
- Item save state (stash 0x52E3F0/0x52E0E0, floors 0x501AC0/0x412B50): new(0x1B0), ctor 0x4D3790,
  capture item->vt[+0x144](state, slot, char) ret 0xC, write 0x4D3070(state, FILE*), read
  0x4D3BC0(state, FILE*, version [0xC16D2C]) ret 8, GUID +0x90; create 0x5FF010(resources, lo, hi, 1, 1, 0)
  ret 0x14 (by RESOURCEGROUP), AddItem, restore vt[+0x148](state) ret 4. Unit (character) save state:
  new(0x278), ctor 0x4B00F0, capture vt[+0x148], restore vt[+0x14C], write 0x4AF830, read 0x4B0600(FILE*, ver).
- Death screen: 0x4DA680 CPlayer::Update (every player-class unit, ret 0xC) -> health <= 0 -> timer +0x7CC
  -> CGameUI::ShowDieMenu 0x54C9D0 (CDieMenu at GameUI+0x2F0, vtable 0xAB69CC, built 0x5730F0), no
  local-player check. Tips 0x55F570(tutorial, id) ret 4 from the same update (3/4/0xB) and from Kill.
  TLMP gates both to the local player's own update.

## Floors, town, resurrect (2026-10-20)
- ChangeLevel 0x40D0E0 only records the request (client +0xFDC dungeon, +0xFD4 mode, +0xFD8 floor, +0xFF8 warp,
  +0x1015 pending); the move happens in the client update. Dungeon "LASTDUNGEON" = player +0x720 name, +0x71C floor.
  Stairs: ('', mode +1/-1, floor 0). Town portal (openable, 0x4E1180): ChangeLevel(L"Town", -99, ...) for EVERY
  registered client (loop over resources+0x14); the return portal passes its own dungeon string, mode 0.
- Die menu CDieMenu (vtable 0xAB69CC): layout 0x572B50 ("Resurrect1..3", "Choice1..3"), Show 0x573210 (slot 6;
  hardcore +0x9C5 hides the resurrect buttons), choice handler 0x572F20: event 0xE here (XP/fame cost),
  0xF floor entrance (gold cost), 0x10 town; close 0x572E60 applies costs, GameUI+0x16A8 = 0/1/2, state 5 ->
  EnterGame(5, ..): 2 -> ChangeLevel(L"Town", -99) (0x4190F9), 1 -> 0x4F1D30 (to the floor entrance).
- 0x4AAF30 is a unit event handler (switch on event+0x14); event 0x71 = warp to level (ChangeLevel at 0x4AB1BD),
  not death.
- TLMP floor ownership: see src/game/floor.h. Town levelId = FNV-1a("TOWN") without seed.

## Spawning (2026-10-20)
- Floor save record (0x501130 write / 0x501BF0 read): +0x34 unit states (0x278), +0x4C item states (0x1B0),
  +0x64 0x68-byte records (0x500E70/0x500F70, name + flags + 3 GUIDs + pos), +0x88 CFormationNodeSaveAndLoad
  (0x40: name, position; only nodes not yet spawned, flag +8).
- Formation nodes: CLevel+0x17C array (data, count, cap) of CFormationNode (vtable 0xAABB8C, ctor 0x50E4E0,
  registers as iLevelUpdate at +0xF0, vtable 0xAABB7C). Update 0x50E5A0 (this = node+0xF0, ?, ?, Vec3* pos)
  ret 0xC: pos within range and +0xF4 clear -> spawn its "Unit Spawner" children, set +0xF4. pos is the local
  player's: groups never wake for other players without help.
- Trigger volumes: CTriggerBox 0x50C6F0 / CTriggerSphere 0x50C8F0 (slot 58, ret 8) call 0x50B890 inside /
  0x50B8E0 outside (thiscall(this, player), ret 4) for every registered client in turn (flicker with 2+
  players). +0xF0 inside, +0xF1/+0xF2 first-time events done, +0x6D events on, +0xF3 active (0x50B850).
  Events via vtable +0x14 (0..3: Triggered / Triggered First Time / Deactivated / Deactivated First Time).
- CUnitSpawner vtable 0xAAC684 (CShape). CLogicTrigger vtable 0xAAB2FC.
- Unit activity: 0x4F7340 (once a second from CLevel::Update 0x4F8B20) runs 0x48BDF0(unit, localPlayerPos)
  for every owned unit: within the activation radius -> +0x162 = 1 and into the update lists (+0x5C via
  0x4F8440, +0x60 via 0x4F85E0), else 0x48C180 (dormant). Items: 0x4D19A0. Monsters far from the LOCAL
  player never think: TLMP re-checks with every other player's position on the floor owner.
- CLevel::UpdateObjects 0x4F1E10: list +0x6C (count +0x70) of iLevelUpdate (formation nodes etc.),
  vt[1](a, dt, pos) -> true removes it. Formation node placement differs per machine (tests, 2026-10-27).

## Quests (2026-10-27)
- One quest manager per game: global [0xF25320] (getter 0x5EFA20; also client+0x34). +8 its player
  (setter 0x5EFA30), +0xC/+0x10 active CQuest* list. Every CPlayer links itself on creation (virtual
  0x4DBC40 -> 0x4D9540(player, manager)): player+0x7B4 = manager, SetPlayer(player), then 0x5F0390
  (active list cleared, every quest re-rolled by 0x5EB430) and the class's starting quests (QUESTS
  types 5/8). The destructor 0x4DDE90 does SetPlayer(0) when player+0x7B4 is set. CPlayer::Update ticks
  the manager (0x5231F0) when +0x7B4 is set. => a remote player's stand-in (CreatePlayer) wiped the
  local quest log, then left the manager without a player when destroyed; travel saved the damage
  (real p1 lost INTROTOGAMEPT2 / RANDOMMINERS on 2026-10-20). TLMP: stand-ins skip 0x4D9540.
- CQuest: +0x18 objectives met (byte), +0x1A active (0x5EB8F0), +0x14 target depth, +0x24 dungeon,
  +0x94 name, +0x1EC 1 active / 2 done, +0x1C8 manager. 0x5EB430(reset, roll=0): target depth =
  [manager+8]'s depth in the dungeon (player +0x9EC list of {wstring dungeon, +0x1C depth}; 0x4DACF0)
  + 1..10, first depth with a level spec (+8). Console QUESTACTIVE: 0x5F1E00(manager, quest, 0, 1).
- Per-quest event 0x5EA1E0(code, arg); manager broadcast 0x5EFA60.
- Spawners: quest +0x154 / +0x164 / +0x174 = vectors of {data, count, cap} of spawner pointers;
  0x5E8FC0(quest, level) runs 0x5F67E0(spawner, level) on each when a level is built (from 0x5F0310 <-
  0x4FAC30). Spawner +0xC quest, +0x10C relative depth (+ quest+0x14), +0x110 absolute depth (-1 =
  quest+0x14), +0x120 placement mode, +0x20 count, +0x125 done. Placement 0x5F5F90 -> 0x5F51A0
  (CQuestUnitData::Spawn). Level +0x140 depth, +0x1F0 dungeon name.

## Session start on the wrong floor (2026-10-27)
- Rare rig flake (1 of 122 host starts): the host started on level "f68eca85" (fast load, ~half the scene
  nodes, saved position in mid-air at y = 100, no layout seed) while the client was on MAIN floor 2.
- Cause: Continue (0x418BC0, mode 1) checks GetAsyncKeyState(VK_SHIFT) at 0x41914D (global: any window)
  and settings value [0xF60D20] (0x5C53D0); with Shift held it calls the loader 0x415A50 from 0x4192B3
  with restore = 1 and "media/testroom.layout": developer test room, restored as saved (0x500BC0 ->
  0x4FF120 with no descriptor skips generation). Normal path: loader from 0x419327, descriptor from
  CDungeon->0x4EDB50(client, floor) after 0x40D250 seeding. Loader args (+0x26C frame): +0x30 / +0x38
  (restore) gate the descriptor path; 0x500BC0 args: wstring path, +0x1C restore flag.
- Reproduced with a real Shift press (rig `key shift down`); fixed by making `je 0x4192BD` at 0x419158
  unconditional (TLMP patch, engine.h kTestRoomShiftCheck). Verified by the shiftstart scenario.

## Treasure rolls (2026-10-27)
- Kill (0x4A9800) rolls only with a killer, victim without tag 0x1C, +0x650 clear. Monsters have a
  treasure table (+0x614): 0x5FF560 then CLevel 0x4F7790 (returns 0x4AACDB). Without one: CLevel
  0x4F7A40 (returns 0x4AAC6E); breakables/chests call 0x4F7A40 from 0x48453D with no killer.
  Rolls often drop nothing. Rig: `expect lootrolls N` counts Kill's rolls since `mark`.
