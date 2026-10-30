#pragma once

// Typed access to Torchlight internals (GOG 1.7.8.70). Every raw address and offset
// lives here; see re/NOTES.md for how each was established.

#include "game/vsstring.h"

#include <cstdint>

namespace tlmp::engine {

struct Vec3
{
  float x, y, z;
};

using Unit = void;   // CCharacter / CPlayer
using Level = void;  // CLevel
using Client = void; // CGameClient

// --- globals / object graph -----------------------------------------------------
constexpr uintptr_t kGameGlobal = 0x00C1AD64; // CGame*
constexpr uint32_t kGameClient = 0x64;        // CGame -> CGameClient*
constexpr uint32_t kClientPlayer = 0x2C;      // CGameClient -> CPlayer* (local)
constexpr uint32_t kClientPet = 0x30;         // CGameClient -> CMonster* (local pet)
constexpr uint32_t kClientLevel = 0x38;       // CGameClient -> CLevel*
constexpr uint32_t kClientPetType = 0x190;    // CGameClient: std::wstring, e.g. "Dog" -- can be empty while
                                              // a pet exists (seen after a floor change); use the pet's GUID
constexpr uint32_t kClientWorldSeed = 0xFB0;  // per-character world seed (from the save); the only
                                              // per-player input to floor layout seeds
constexpr uint32_t kClientGameUI = 0x3C;      // CGameClient -> CGameUI*
constexpr uint32_t kGameUIConsole = 0x13EC;   // CGameUI -> CConsole*
constexpr uint32_t kClientResources = 0x230;  // CGameClient -> its CResourceManager (== player+0x60)
constexpr uint32_t kClientSize = 0x2484;      // new(0x2484) in 0x402100
constexpr uint32_t kClientState = 0xF80;      // set to 3 by SetPlayerClass; NOT a gameplay flag (0 in town too)
constexpr uint32_t kLevelUnitFactory = 0xD8;  // CLevel -> object CreatePlayer is called on
constexpr uint32_t kLevelUnits = 0x64;        // CLevel -> list object; [0] = head node; node { unit*, next* }.
                                              // The level owns these units (its dtor deletes them).
constexpr uint32_t kLevelWorldSeed = 0x194;   // CLevel: copy of client+0xFB0 used in the layout seed
constexpr uint32_t kLevelName = 0x1F0;        // CLevel: std::wstring, e.g. "TOWN"
constexpr uint32_t kUnitNode = 0x58;          // CBaseUnit -> Ogre::OctreeNode*
constexpr uint32_t kUnitPosition = 0x70;      // CBaseUnit: Vec3 current position
constexpr uint32_t kUnitTransform = 0xAC;     // CBaseUnit: Ogre::Matrix4 (row-major; translation in [i][3])
constexpr uint32_t kUnitName = 0x420;         // CBaseUnit: std::wstring display name (set by 0x40C630)
constexpr uint32_t kUnitAIState = 0x284;      // CCharacter: int AI state; 5 = dying, 6 = dead (Kill ignores both)
constexpr int kAIStateDying = 5;
constexpr int kAIStateDead = 6;
constexpr uint32_t kUnitAlignment = 0x28C;    // CBaseUnit: raw alignment (read via GetAlignment)
// CCharacter base stats (console STRENGTH etc. 0x488410..; class defaults 0x4D8580): strength +0x3AC,
// dexterity +0x3A8, magic +0x3B4, defense +0x3B0, base armor +0x3A4; unspent points +0x3DC.
constexpr uint32_t kCharStrength = 0x3AC;
constexpr uint32_t kCharDexterity = 0x3A8;
constexpr uint32_t kCharMagic = 0x3B4;
constexpr uint32_t kCharDefense = 0x3B0;
constexpr uint32_t kCharBaseArmor = 0x3A4;
constexpr uint32_t kUnitLevel = 0xF0;         // CCharacter: int level (CMonster ctor 0x4D5C60 stores max(arg, existing))
constexpr uint32_t kUnitHealth = 0x394;       // CCharacter: float current health (max = int +0x398 + int +0x39C)
// 0x4901E0: CCharacter::GetMaxHealth() -- (base +0x398 + +0x39C) scaled by stat 0x14 (%) plus stat 5
// (flat); AdjustHealth clamps to it.
constexpr uint32_t kUnitBaseHealth = 0x398;
constexpr uint32_t kUnitBonusHealth = 0x39C;
using GetMaxHealthFn = int(__thiscall*)(Unit* unit);
inline const auto GetMaxHealth = reinterpret_cast<GetMaxHealthFn>(0x004901E0);
constexpr uint32_t kUnitMoving = 0x208;       // CCharacter: bool, set while following a path
constexpr uint32_t kUnitResources = 0x60;     // CBaseUnit -> CResourceManager (CreateItem's this)
// CResourceManager keeps the game clients it serves: items at +0x14, count +0x18, capacity +0x1C
// (capacity 10 at runtime). Entry 0 is the local client (CGameClient ctor pushes itself, 0x40F970).
// Trigger volumes (0x50C8F0), warpers and openables (0x4E1180) loop over every entry's player
// (+0x2C); most other code only reads entry 0.
constexpr uint32_t kResClients = 0x14;
constexpr uint32_t kResClientCount = 0x18;
constexpr uint32_t kResClientCapacity = 0x1C;
constexpr uint32_t kUnitInventory = 0x404;    // CCharacter -> CInventory*; slots 0..11 = equipment
constexpr uint32_t kUnitGuid = 0x168;         // CBaseUnit: uint64 UNIT_GUID of its definition (stable id)
constexpr uint32_t kItemName = 0x1B0;         // CItem: std::wstring DISPLAYNAME, e.g. "Epic Rusty Blade"
                                              // (CreateItem needs the internal NAME, so we use the GUID)
constexpr uint32_t kEnforceRequirements = 0x40C; // CCharacter byte: CEquipment::MeetsRequirements (0x4B8660)
                                                 // checks level/stats only when set; otherwise the game
                                                 // moves gear the character can't use to the backpack
constexpr uint32_t kPlayerClassName = 0x30;   // CPlayer: std::wstring, e.g. "Destroyer"
constexpr uint32_t kPlayerMinions = 0x5C4;   // CPlayer: std::vector<CCharacter*> pet + summons (begin, end +4,
                                              // capacity +8); SaveCharacter (0x417380) saves every one of them
constexpr uint32_t kMonsterMode = 0x65C;      // CMonster: SetPetType (0x40CDE0) sets 2 for the pet
constexpr int kMonsterModePet = 2;

// --- functions ---------------------------------------------------------------------
// 0x5FE5D0: looks className up in the "PLAYERS" table, new(0xA10), ctor, vtable[+0xF4] init.
using CreatePlayerFn = Unit*(__thiscall*)(void* factory, const wchar_t* className, int arg);
// 0x5FE830: looks name up in the "MONSTERS" table, new(0x728) CMonster, init. level < 1
// means "use the factory's current level". SetPetType calls it as (petType, 0, 0).
using CreateMonsterFn = Unit*(__thiscall*)(void* factory, const wchar_t* name, int level, int arg);
// 0x5FE580: CreateItem -- "ITEMS" table lookup + dynamic_cast to CItem. Starter items use
// (name, 0, 1) on the owning unit's +0x60 resource manager.
using CreateItemFn = void*(__thiscall*)(void* resources, const wchar_t* name, int level, int arg);
// 0x4E7890: CInventory::GetItemInSlot(slot) -> CItem* or null.
using GetItemInSlotFn = void*(__thiscall*)(void* inventory, int slot);
// 0x4E9480: CInventory::PlaceItemInSlot(item, slot, flag) -- equips for slots < 12. Returns the
// item on success, null if refused (occupied slot or wrong item type).
using PlaceItemInSlotFn = void*(__thiscall*)(void* inventory, void* item, int slot, int flag);
// 0x4857A0: GetAlignment -- +0x28C, with 1 and 2 swapped while the unit has tag 0x50 (charmed).
// KILLALL (0x4EF080) treats non-player characters with alignment 2 or 4 as hostile.
using GetAlignmentFn = int(__thiscall*)(Unit* unit);
inline const auto GetAlignment = reinterpret_cast<GetAlignmentFn>(0x004857A0);
inline bool isHostileAlignment(int a)
{
  return a == 2 || a == 4;
}
// 0x4857E0: stores alignment at +0x28C and picks the outline (1 = player/friendly).
using SetAlignmentFn = void(__thiscall*)(Unit* unit, int alignment);
// 0x4F57D0: registers the unit with the level (owned list +0x64) at pos.
using AddUnitFn = Unit*(__thiscall*)(Level* level, Unit* unit, const Vec3* pos, bool flag);
// 0x4F7160: unlinks the unit from the level's lists (+0x5C, +0x64, +0x80); does not free it.
using RemoveUnitFn = void(__thiscall*)(Level* level, Unit* unit, bool stopActions);
// 0x494B30: click-to-move: A* path to (x, z) using the unit's pathfinder (+0x20C).
using MoveToFn = void(__thiscall*)(Unit* unit, Level* level, float x, float z);
// vtable[10] (+0x28): set position.
using SetPositionFn = void(__thiscall*)(Unit* unit, float x, float y, float z);
// vtable[0]: scalar deleting destructor.
using DeleteFn = void*(__thiscall*)(Unit* unit, unsigned flags);

inline const auto CreatePlayer = reinterpret_cast<CreatePlayerFn>(0x005FE5D0);
inline const auto CreateMonster = reinterpret_cast<CreateMonsterFn>(0x005FE830);
// 0x4E9CA0: CInventory::RemoveItem(item) -- unlinks it (unequipping if in slots 0..11) but
// does not free the item.
using RemoveItemFn = bool(__thiscall*)(void* inventory, void* item);
inline const auto RemoveItem = reinterpret_cast<RemoveItemFn>(0x004E9CA0);
// 0x5FE4F0: CreateUnitByGuid(lo, hi, initArg, flag1ae, equipFlag) on a resource manager.
// CreateItem(name, 0, 1) is equivalent to (initArg 0, flag1ae 1, equipFlag 0).
using CreateUnitByGuidFn = void*(__thiscall*)(void* resources, uint32_t lo, uint32_t hi, int initArg, char flag1ae,
                                              char equipFlag);
inline const auto CreateUnitByGuid = reinterpret_cast<CreateUnitByGuidFn>(0x005FE4F0);
inline const auto CreateItem = reinterpret_cast<CreateItemFn>(0x005FE580);
inline const auto GetItemInSlot = reinterpret_cast<GetItemInSlotFn>(0x004E7890);
inline const auto PlaceItemInSlot = reinterpret_cast<PlaceItemInSlotFn>(0x004E9480);
inline const auto SetAlignment = reinterpret_cast<SetAlignmentFn>(0x004857E0);
inline const auto AddUnit = reinterpret_cast<AddUnitFn>(0x004F57D0);
inline const auto RemoveUnit = reinterpret_cast<RemoveUnitFn>(0x004F7160);
inline const auto MoveTo = reinterpret_cast<MoveToFn>(0x00494B30);

// Global RNG seed: cdecl void(DWORD seed); state at 0xEDE788 (+ counter 0xEDE78C), seed 0
// means GetTickCount(). Level layout is seeded at 0x4FF6DC (in 0x4FF120) with
// level[+0x140] + level[+0x194] + ([[level+0x190]+0x242C] ? that+0x38 : 0).
constexpr uintptr_t kSeedRng = 0x005BFD70;
constexpr uintptr_t kSeedState = 0x00EDE788;
constexpr uintptr_t kLayoutSeedReturn = 0x004FF6E1; // return address of the layout seed call; ESI = CLevel*
// Continue (0x418BC0, mode 1): with Shift held (GetAsyncKeyState, global -- any window) and settings
// value [0xF60D20] set, the game loads the developer room media/testroom.layout, restored as saved (no
// layout seed), instead of the save's floor. At 0x419158 `je 0x4192BD` (0F 84 5F 01 00 00) skips
// that when Shift is up; TLMP makes it unconditional (E9 60 01 00 00 90).
constexpr uintptr_t kTestRoomShiftCheck = 0x00419158;

// CGameClient::SaveCharacter, __thiscall(client, bool, bool). Copies client+0xFB0 into the
// save record, among everything else ("Saving character to :" in Ogre.log).
constexpr uintptr_t kSaveCharacter = 0x00417380;

// --- UI / console / settings --------------------------------------------------------------
// CKeyManager at CGameClient+0x240: the game's key state, polled by its hotkeys. The window
// procedure (0x40DE20) gives each key message to CEGUI (GameUI 0x53FE70: injectKeyDown/Up/Char)
// and then to 0x4E7120 OnMessage(msg, vk), which sets down +0x909[vk] and the pressed / released
// edges +0x709 / +0xB09; 0x4E7220 moves the edges to +0x9 / +0x409 each frame. Queries: pressed
// 0x4E71B0, down (or pressed) 0x4E71D0, released 0x4E7200.
constexpr uint32_t kClientKeys = 0x240;
constexpr uintptr_t kKeyManagerOnMessage = 0x004E7120;
constexpr uint32_t kKeysDown = 0x909;
// 0x53FC90: CGameUI::ShowSmallMessage(std::wstring text [by value], float seconds). Shown in
// the HUD's SmallMessageText window (+0x90), held for `seconds`, then faded.
using ShowSmallMessageFn = void(__thiscall*)(void* gameUI, vs::WString text, float seconds);
inline const auto ShowSmallMessage = reinterpret_cast<ShowSmallMessageFn>(0x0053FC90);
// 0x56C460: CConsole::Print(std::wstring line [by value]).
using ConsolePrintFn = void(__thiscall*)(void* console, vs::WString line);
inline const auto ConsolePrint = reinterpret_cast<ConsolePrintFn>(0x0056C460);
// 0x56CD00: CConsole::Execute(std::wstring line [by value]) -- the command dispatcher (ret 0x1C).
constexpr uintptr_t kConsoleExecute = 0x0056CD00;

// Settings values live in CSettings ([[0xE37968]+0x48]) as a uint32 vector at +0x80, indexed by
// ids the game registers at startup and keeps in globals.
constexpr uintptr_t kMasterResourceManager = 0x00E37968;
constexpr uint32_t kMrmSettings = 0x48;
constexpr uint32_t kSettingsValues = 0x80;      // begin; +0x84 = end
constexpr uintptr_t kSettingConsoleId = 0x00F60D20;        // "CONSOLE" (enables the console key)
constexpr uintptr_t kSettingConsoleNoPauseId = 0x00F60D34; // "CONSOLE NOPAUSE"

// 0x5C5530: CDynamicPropertyFile writer, __thiscall(this, path, flag) ret 8 -> bool. CSettings
// saves settings.txt / local_settings.txt through it (via Save 0x5C5C80, a virtual).
constexpr uintptr_t kPropertyFileWrite = 0x005C5530;

// --- monsters ---------------------------------------------------------------------------
// 0x4A9800: CCharacter::Kill(killer, const Vector3* dir, float force, char flag), ret 0x10. It is
// vtable slot 90 (+0x168) for monsters; CPlayer::Kill (0x4DAA80) forwards to it. KILLALL calls
// it as (0, 0, 0, 0).
constexpr uintptr_t kCharacterKill = 0x004A9800;
using KillFn = void(__thiscall*)(Unit* unit, int killer, const Vec3* dir, float force, char flag);
// 0x4D7CB0: CMonster::UpdateAI(float dt, char force), ret 8; vtable slot 95. Skipped entirely
// when the AIFREEZE setting is on.
constexpr uintptr_t kMonsterUpdateAI = 0x004D7CB0;

// --- death screen --------------------------------------------------------------------------------
// 0x4DA680 CPlayer::Update(?, ?, float dt), ret 0xC, runs for EVERY player-class unit. Once its
// health is <= 0 a timer (+0x7CC) runs out and it calls CGameUI::ShowDieMenu (0x54C9D0, thiscall,
// no args; the menu is CDieMenu at GameUI+0x2F0) -- without checking that it is the local player.
// The same update shows tutorial tips (0x55F570(tutorial, int id), ret 4: 3 low health, 4 low mana,
// 0xB pet) for any player; Kill (0x4AB3C8) shows one too. CPlayer::Kill (0x4DAA80) saves the game
// when the character is hardcore (+0x9C5).
constexpr uintptr_t kPlayerUpdate = 0x004DA680;
constexpr uintptr_t kShowDieMenu = 0x0054C9D0;
constexpr uintptr_t kShowTip = 0x0055F570;
constexpr uint32_t kPlayerHardcore = 0x9C5;

// --- items on the ground -----------------------------------------------------------------------
// 0x4F5960 CLevel::AddItem(item, const Vec3* pos, char flag) -> item, ret 0xC (ground items live in
// the list at level +0x68). 0x4F7260 CLevel::RemoveItem(item, char hide) -> bool, ret 8: unlinks
// from +0x68/+0x60/+0x7C, does not free. 0x498A20 CCharacter::PickUp(item, level), ret 8:
// RemoveItem(item, 1), then into the inventory (0x4E9A30) or gold.
constexpr uintptr_t kLevelAddItem = 0x004F5960;
constexpr uintptr_t kPickUp = 0x00498A20;
using AddItemFn = void*(__thiscall*)(Level* level, void* item, const Vec3* pos, char flag);
using RemoveItemFromLevelFn = bool(__thiscall*)(Level* level, void* item, char hide);
using PickUpFn = uint32_t(__thiscall*)(Unit* character, void* item, Level* level);
inline const auto AddItemToLevel = reinterpret_cast<AddItemFn>(kLevelAddItem);
inline const auto RemoveItemFromLevel = reinterpret_cast<RemoveItemFromLevelFn>(0x004F7260);
inline const auto PickUp = reinterpret_cast<PickUpFn>(kPickUp);
// The item held on the mouse cursor: CGameUI +0x60 (0x54D830 puts it back into the inventory,
// or on the ground at the player when there is no room, then clears it).
constexpr uint32_t kGameUIHeldItem = 0x60;

// Item save state, as the shared stash (0x52E3F0 save / 0x52E0E0 load) and floors use it:
// new(0x1B0), ctor 0x4D3790; item->vtable[+0x144](state, int slot, char) captures (ret 0xC);
// 0x4D3070(state, FILE*) writes (ret 4); 0x4D3BC0(state, FILE*, version) reads (ret 8), version =
// [0xC16D2C] (written first in sharedstash.bin); GUID at +0x90/+0x94. Restored with
// 0x5FF010(resources = client+0x230, lo, hi, 1, 1, 0) (create by GUID: items, monsters by
// RESOURCEGROUP; ret 0x14), then item->vtable[+0x148](state) (ret 4).
constexpr uint32_t kItemStateSize = 0x1B0;
using ItemStateCtorFn = void*(__thiscall*)(void* state);
using ItemStateWriteFn = void(__thiscall*)(void* state, void* file);
using ItemStateReadFn = void(__thiscall*)(void* state, void* file, uint32_t version);
inline const auto ItemStateCtor = reinterpret_cast<ItemStateCtorFn>(0x004D3790);
inline const auto ItemStateWrite = reinterpret_cast<ItemStateWriteFn>(0x004D3070);
inline const auto ItemStateRead = reinterpret_cast<ItemStateReadFn>(0x004D3BC0);
constexpr uintptr_t kItemStateVersion = 0x00C16D2C;
constexpr uint32_t kItemStateGuid = 0x90;
constexpr int kItemCaptureSlot = 81; // vtable +0x144
constexpr int kItemRestoreSlot = 82; // vtable +0x148
// Character save state (floors 0x501B50 save / 0x412B50 load, the player in SaveCharacter): new(0x278),
// ctor 0x4B00F0; unit->vtable[+0x148](state) captures (CCharacter 0x4A4940, ret 4); 0x4AF830(state,
// FILE*) writes; 0x4B0600(state, FILE*, version) reads (ret 8), version = [0xC128C0] (first dword of
// a character save). UNIT_GUID at +0x18, level int +0xD8, position +0x74, +0x10 = not placed in the
// level. Restored with 0x5FF010(resources, lo, hi, level, 1, 0), AddUnit at +0x74, then
// unit->vtable[+0x14C](state) (CCharacter 0x4AC7C0, ret 4).
constexpr uint32_t kCharStateSize = 0x278;
inline const auto CharStateCtor = reinterpret_cast<ItemStateCtorFn>(0x004B00F0);
inline const auto CharStateWrite = reinterpret_cast<ItemStateWriteFn>(0x004AF830);
inline const auto CharStateRead = reinterpret_cast<ItemStateReadFn>(0x004B0600);
constexpr uintptr_t kCharStateVersion = 0x00C128C0;
constexpr uint32_t kCharStateGuid = 0x18;
constexpr uint32_t kCharStateLevel = 0xD8;
constexpr uint32_t kCharStatePosition = 0x74;
constexpr int kCharCaptureSlot = 82; // vtable +0x148
constexpr int kCharRestoreSlot = 83; // vtable +0x14C
using CreateByGuidFn = void*(__thiscall*)(void* resources, uint32_t lo, uint32_t hi, int level, int a, int b);
inline const auto CreateByGuid = reinterpret_cast<CreateByGuidFn>(0x005FF010);

// --- quests ----------------------------------------------------------------------------------------
// CQuestManager at client+0x34: active CQuest* array at +0xC (count +0x10); saved by 0x5F0C20 (each
// quest by name, then 0x5E9750: target UNIT_GUID of +0x1D4, +0x10, +0x14, +0x1EC (1 active, 2 done?),
// unit lists +0xD0/+0xE0/+0xF0). Console QUESTACTIVE / QUESTCOMPLETE / QUESTRESET <name>.
// CQuestUnitData::Spawn 0x5F51A0 (thiscall(questUnit, resources, const Vec3* pos) -> unit, ret 8):
// creates the quest's unit (0x5FF150), adds it to [resources+0xC] (0x4F70E0), then tags it: quest GUID
// (quest +0x1C0/+0x1C4) at unit +0x138/+0x13C, quest-unit index (questUnit +8) at +0x140; champions get
// "CHAMPION". 0x5F5130 matches a unit to a quest unit by those tags; 0x4818F0 "is a quest unit" (tags
// set, or tag 0x67). Unit save states keep the tags (0x4A4940 / 0x4AC7C0). Kill's "KILL" event
// (0x5E8140, quest event code, killer must be a player) is what quests count.
constexpr uintptr_t kQuestUnitSpawn = 0x005F51A0;
// The game's one quest manager (global [0xF25320], getter 0x5EFA20) serves a single player: +8 holds
// it (setter 0x5EFA30). Every CPlayer links itself on creation (virtual 0x4DBC40 calls
// 0x4D9540(player, manager)): player+0x7B4 = manager, manager+8 = player, then 0x5F0390 resets the
// manager -- active list cleared, every quest re-rolled (0x5EB430) -- and the class's starting
// quests are added. The destructor (0x4DDE90) sets manager+8 = 0 when player+0x7B4 is set.
// CPlayer::Update ticks the manager (0x5231F0) when +0x7B4 is set; CTriggerUnit::Interact reads
// the user's +0x7B4 unchecked for quest requirements (0x4E1D55). A quest's target depth (+0x14) is
// [manager+8]'s depth in the quest's dungeon (player +0x9EC list, depth at +0x1C) plus 1..10.
constexpr uintptr_t kPlayerLinkQuests = 0x004D9540; // thiscall(player, manager), ret 4
constexpr uint32_t kPlayerQuestManager = 0x7B4;
constexpr uint32_t kUnitQuestGuid = 0x138;  // uint64, all ones when none
constexpr uint32_t kUnitQuestIndex = 0x140;

// --- spawning ------------------------------------------------------------------------------------
// Trigger volumes (CLogicTrigger: CTriggerBox vtable 0xAAB5EC, CTriggerSphere 0xAAB6E4) update in
// vtable slot 58 (box 0x50C6F0, sphere 0x50C8F0; thiscall(this, ?, ?), ret 8): for EVERY registered
// client's player they call 0x50B890 (inside) or 0x50B8E0 (outside), thiscall(this, player), ret 4.
// State: +0xF0 inside, +0xF1/+0xF2 the "first time" events done, +0x6D events on, +0xF3 active.
constexpr uintptr_t kTriggerBoxUpdate = 0x0050C6F0;
constexpr uintptr_t kTriggerSphereUpdate = 0x0050C8F0;
constexpr uintptr_t kTriggerVolumeInside = 0x0050B890;
constexpr uintptr_t kTriggerVolumeOutside = 0x0050B8E0;
// Formation nodes (CFormationNode, ctor 0x50E4E0): the monster groups placed in a floor, kept in the
// array at CLevel+0x17C (data, count +4, capacity +8). Each is a level-update object at +0xF0 whose
// update 0x50E5A0 (thiscall(node+0xF0, ?, ?, const Vec3* playerPos), ret 0xC) spawns the group (its
// "Unit Spawner" children) once the LOCAL player's position comes within range, then sets
// +0xF4 (spawned). A floor save keeps only the groups not spawned yet (CFormationNodeSaveAndLoad,
// 0x5014B0 / 0x50E2C0: name, position), and a load recreates just those (0x4141E7).
// Unit activity: once a second (0x4F7340, from CLevel::Update 0x4F8B20) every unit the level owns is
// checked by 0x48BDF0 (thiscall(unit, const Vec3* pos), ret 4) against the LOCAL player's position:
// within the activation radius (settings +0xC0/+0xC4) -> +0x162 = 1 and into the update lists
// (+0x5C/+0x60), else deactivated (0x48C180). Inactive monsters don't think, move or fight.
// Players, pets of active owners and dying units are always active. ("Active Units" in DISPLAY STATS.)
constexpr uintptr_t kUnitActivity = 0x0048BDF0;
constexpr uint32_t kUnitActive = 0x162;
constexpr uint32_t kLevelFormationNodes = 0x17C;
constexpr uint32_t kNodeSpawned = 0xF4;
constexpr uintptr_t kNodeUpdate = 0x0050E5A0;
// 0x510EB0 CPositionableObject::GetPosition(Vec3* out, char world) -> out, ret 8.
using GetPositionFn = Vec3*(__thiscall*)(const void* object, Vec3* out, char world);
inline const auto GetPosition = reinterpret_cast<GetPositionFn>(0x00510EB0);

// --- animation -----------------------------------------------------------------------------------
// Every unit has an animation object at +0x1AC (thiscall this of the calls below). Its animation set
// (+0x1D4) lists the animation names: count +0x28, std::string (char) entries of 0x1C bytes from
// +0x38 (end +0x3C), compared upper-cased (0x5D1840). Calls take an index into that list; one-shot
// animations (attacks, casts: loop = 0) are followed by a queued one (IDLE or a follow-up):
//   0x4C8AD0 play(index, char loop, float rate, float duration), ret 0x10
//   0x4CA8E0 crossfade(index, char loop, float blend, float rate, float duration), ret 0x14
//            (CCharacter wrapper 0x486070; attacks 0x496EB0 crossfade the swing, then queue)
//   0x4C8950 queue(index, char loop, float blend, float rate), ret 0x10
// duration 0x0A91FB0's value means "the animation's own length". 0x4C3CC0 is name -> index.
constexpr uint32_t kUnitAnimation = 0x1AC;
constexpr uint32_t kAnimationSet = 0x1D4;
constexpr uint32_t kAnimSetCount = 0x28;
constexpr uint32_t kAnimSetNames = 0x38;
constexpr uint32_t kAnimNameSize = 0x1C;
constexpr uintptr_t kAnimPlay = 0x004C8AD0;
constexpr uintptr_t kAnimCrossfade = 0x004CA8E0;
constexpr uintptr_t kAnimQueue = 0x004C8950;

// --- skills --------------------------------------------------------------------------------------
// 0x496EB0: CCharacter::UseSkill(guidLo, guidHi) -> 0 when refused (unknown skill, no mana, ...),
// ret 8; guid -1 = the current skill. Player input (0x414790, CGameClient), the AI and the UI all
// come through here. Player input first aims: 0x486180(x, z) sets the target point (+0x1CC, and
// the direction +0x1D8), 0x4949D0(unit) the target unit (+0x290). Skills live in the character's
// CSkillManager (+0x184): 0x5DC320 FindSkillByGuid(lo, hi), 0x5DD660 FindSkill(name). CSkill +0x180
// GUID (uint64). Character: +0x2D0 current CSkill*, +0x2E0 / +0x2E8 / +0x2F0 skill GUIDs of the
// mouse slots (right, alternate, ...), +0x860 the ten hotkey slots (GUID pairs).
using UseSkillFn = uint32_t(__thiscall*)(Unit* unit, uint32_t guidLo, uint32_t guidHi);
constexpr uintptr_t kUseSkill = 0x00496EB0;
using AimAtFn = void(__thiscall*)(Unit* unit, float x, float z);
inline const auto AimAt = reinterpret_cast<AimAtFn>(0x00486180);
using SetTargetUnitFn = void(__thiscall*)(Unit* unit, Unit* target);
inline const auto SetTargetUnit = reinterpret_cast<SetTargetUnitFn>(0x004949D0);
using FindSkillByGuidFn = void*(__thiscall*)(void* skillManager, uint32_t guidLo, uint32_t guidHi);
inline const auto FindSkillByGuid = reinterpret_cast<FindSkillByGuidFn>(0x005DC320);
constexpr uint32_t kCharSkillManager = 0x184;
// CSkillManager: every skill the character has (its class's, invested or not): CSkill* array at
// +0x3C, count +0x40. 0x5DE550 SetSkillLevel(CSkill*, level) is what spending a skill point calls
// (passives, type 4 at CSkill +0x38, get their effects through it). 0x4805A0 LearnSkill(name*, int).
constexpr uint32_t kSkillManagerSkills = 0x3C;
using SetSkillLevelFn = void(__thiscall*)(void* manager, void* skill, int level);
inline const auto SetSkillLevel = reinterpret_cast<SetSkillLevelFn>(0x005DE550);
using FindSkillByNameFn = void*(__thiscall*)(void* manager, const void* name /* VS2008 std::wstring* */);
inline const auto FindSkillByName = reinterpret_cast<FindSkillByNameFn>(0x005DD660);
using LearnSkillFn = int(__thiscall*)(Unit* unit, const void* name /* VS2008 std::wstring* */, int unused);
inline const auto LearnSkill = reinterpret_cast<LearnSkillFn>(0x004805A0);
constexpr uint32_t kCharCurrentSkill = 0x2D0;
constexpr uint32_t kCharRightSkill = 0x2E0;
constexpr uint32_t kCharHotkeySkills = 0x860; // 10 x uint64
constexpr uint32_t kCharSkillTarget = 0x1CC;  // Vec3
constexpr uint32_t kCharTargetUnit = 0x290;
constexpr uint32_t kSkillGuid = 0x180;

// --- damage, experience ------------------------------------------------------------------------
// 0x499750: CCharacter::TakeDamage, the target-side funnel for weapon hits (0x4A2380 computes
// the roll, armor and crits, then calls it), missiles (0x50DA60), effects (0x5E2270, 0x4AAF30):
//   (target, CLevel* level, const Vec3* sourcePos, float damage (> 0), const Vec3* dir, float force,
//    CCharacter* attacker (may be null), char showNumber, char react, char knockback, int, uint flags)
// ret 0x2C. Returns at once for damage 0 or a dying/dead target. Calls AdjustHealth(-damage); at
// 0 health: target->Kill (vtable +0x168) with attacker, then attacker->GiveExperience(level,
// target, [target+0x3D4], 0) and GiveFame(.., [target+0x3D8], 0) and zeroes both. react: the level
// and target are told about the attacker (aggro, 0x4F1D60 / vtable +0x178), and, unless
// flags & 0x300, reflected damage is dealt back to the attacker (recursive call). knockback
// pushes along dir with force. level = [[unit+0x60]+0xC] at the callers.
constexpr uintptr_t kTakeDamage = 0x00499750;
using TakeDamageFn = void(__thiscall*)(Unit* target, Level* level, const Vec3* sourcePos, float damage,
                                       const Vec3* dir, float force, Unit* attacker, char showNumber, char react,
                                       char knockback, int arg11, uint32_t flags);
inline const auto TakeDamage = reinterpret_cast<TakeDamageFn>(kTakeDamage);
constexpr uint32_t kDamageNoReflect = 0x300;
// 0x490420: CCharacter::GiveExperience(level, victim, int amount, char raw), ret 0x10. Not raw:
// adds the receiver's XP bonus and, for a minion (+0x5B4 owner), forwards to the owner. Passes
// a raw share to every minion (+0x5C4), adds to +0x3C8, levels up via vtable +0x160.
// 0x4880E0: GiveFame, same shape (+0x3CC).
constexpr uintptr_t kGiveExperience = 0x00490420;
constexpr uintptr_t kGiveFame = 0x004880E0;
using GiveRewardFn = void(__thiscall*)(Unit* unit, Level* level, Unit* victim, int amount, char raw);
inline const auto GiveExperience = reinterpret_cast<GiveRewardFn>(kGiveExperience);
inline const auto GiveFame = reinterpret_cast<GiveRewardFn>(kGiveFame);
constexpr uint32_t kMinionOwner = 0x5B4;  // CCharacter: the master of a minion or pet (SetMaster 0x499E70)
constexpr uint32_t kUnitXpReward = 0x3D4;   // CCharacter: XP its killer gets (TakeDamage zeroes it)
constexpr uint32_t kUnitFameReward = 0x3D8; // CCharacter: fame its killer gets

// --- doors / levers / other openables (CTriggerUnit, derives CItem) ------------------------
// Data-driven props with numbered animation states (UNITTYPE OPENABLE, MAXSTATES, ANIMATION_STATE_n).
constexpr uint32_t kTriggerInteractable = 0x19C; // byte: can be used (cleared at the last state)
constexpr uint32_t kTriggerState = 0x1EC;        // int current state
constexpr uint32_t kTriggerMaxState = 0x1F4;     // int MAXSTATES - 1
constexpr uint32_t kTriggerLoops = 0x1F8;        // int, nonzero = cycles instead of stopping
// 0x4E1CC0: CTriggerUnit::Interact(CCharacter* user), vtable slot 79 (+0x13C): requirement
// checks (keys, quests) then Activate.
constexpr uintptr_t kTriggerInteract = 0x004E1CC0;
using TriggerInteractFn = uint32_t(__thiscall*)(Unit* trigger, Unit* user);
// The town portal a Town Portal spell opens (CTriggerUnit, at the caster): UNIT_GUID.
constexpr uint64_t kTownPortalGuid = 0xd3a8f99f2fa111deull;
// 0x4E1180: CTriggerUnit::Activate(CCharacter* user), ret 4: one state step + logic events;
// user may be null.
using TriggerActivateFn = void(__thiscall*)(Unit* trigger, Unit* user);
inline const auto TriggerActivate = reinterpret_cast<TriggerActivateFn>(0x004E1180);
constexpr uintptr_t kTriggerActivate = 0x004E1180;
// Doors etc. are not in the CLevel unit lists (+0x5C/+0x64/+0x80 all came up empty in a dungeon),
// so TLMP tracks them itself between these two:
constexpr uintptr_t kTriggerInit = 0x004E2030; // vtable slot 61 init(descriptor, arg), ret 8
constexpr uintptr_t kTriggerDtor = 0x004E09C0; // destructor body (0x4E1160 deleting dtor calls it)
// 0x4F57D0 CLevel::AddUnit(unit, const Vec3* pos, char flag), ret 0xC.
constexpr uintptr_t kAddUnit = 0x004F57D0;

// --- level changes --------------------------------------------------------------------------
// 0x40D0E0: CGameClient::ChangeLevel(std::wstring dungeon, int mode, int floor, char waypoint,
// std::wstring warpName, char flag), both strings by value, ret 0x48. Every map change goes
// through it: CWarper (0x50CBE9, called once per registered client), openable props such as
// the town portal (0x4E1540/0x4E1686), the waypoint menu (0x5B6481), console ASCEND/DESCEND,
// death (0x4AB1BD). Console DESCEND passes (dungeon, 1, level+0x140 + 1, 0, L"", 1).
constexpr uintptr_t kChangeLevel = 0x0040D0E0;
using ChangeLevelFn = void(__thiscall*)(void* client, vs::WString dungeon, int mode, int floor, char waypoint,
                                        vs::WString warpName, char flag);
// 0x418BC0: CGameClient enter-game routine, __thiscall(client, a, b) ret 8 (loads the saved
// level; may call ChangeLevel(L"Town", -99, 0, 0, L"", 0) at 0x4190F9).
constexpr uintptr_t kEnterGame = 0x00418BC0;
// Return address of the only enter-game call made at startup (0x41A630 builds the title
// backdrop through it); every other call (0x41B9B8, 0x41BA88, 0x41BAEA in the client update)
// means the player has entered the game.
constexpr uintptr_t kEnterGameAtStartup = 0x0041AB2C;
// Its first argument is 5 for the resurrect pass after the die menu (0x572E60 stores the choice in
// GameUI+0x16A8: 0 here, 1 floor entrance (0x4F1D30), 2 town (ChangeLevel L"Town", -99)).
constexpr int kEnterGameResurrect = 5;
constexpr uint32_t kLevelFloor = 0x140;          // CLevel: floor index within its dungeon
constexpr uint32_t kClientDungeon = 0x242C;      // CGameClient -> CDungeon*
constexpr uint32_t kDungeonName = 0x3C;          // CDungeon: std::wstring, e.g. "MAIN"

// Monsters can't be made with CreateUnitByGuid: its factory 0x5FE210 only builds the CREATEAS
// types EQUIPMENT/BREAKABLE/TRIGGER/GOLD. CreateMonster 0x5FE830 looks the name up in "MONSTERS",
// then new(0x728), ctor 0x4D5C60(this=mem, factory, level), vtable+0xF4 init(descriptor, arg).
// createMonsterByGuid repeats those steps with the GUID lookup instead of the name lookup.
constexpr uintptr_t kUnitResourceList = 0x00E44500;  // CUnitResourceList* (getter 0x530FF0)
using LookupGuidFn = void*(__thiscall*)(void* list, uint32_t lo, uint32_t hi); // 0x5321A0, ret 8
using MonsterCtorFn = Unit*(__thiscall*)(void* mem, void* factory, int level);  // 0x4D5C60, ret 8
using InitFromDescriptorFn = void(__thiscall*)(Unit* unit, void* descriptor, int arg); // vtable slot 61
constexpr uint32_t kMonsterSize = 0x728;

// 0x40CDE0: CGameClient::SetPetType(std::wstring type [by value]), ret 0x1C -- creates the pet
// (CreateMonster), names it from GameUI+0x1418, pet mode, adds it to the level.
using SetPetTypeFn = void(__thiscall*)(void* client, vs::WString type);
inline const auto SetPetType = reinterpret_cast<SetPetTypeFn>(0x0040CDE0);
// 0x4ABD60: CPlayer::AddMinion(CCharacter* minion), ret 4 -- appends to the minion list
// (+0x5C4) unless already there, retiring excess summons of the same type. SetPetType does NOT
// call it: a pet missing from the list is saved as an ordinary floor unit, not as the character's
// pet. Loading (0x418E70, 0x41970C) and console FIXPET (0x56D62D) do: pet+0x50E = 1 (INVINCIBLE),
// mode 2, name from GameUI+0x1418, then AddMinion.
using AddMinionFn = void(__thiscall*)(Unit* player, Unit* minion);
inline const auto AddMinion = reinterpret_cast<AddMinionFn>(0x004ABD60);
constexpr uint32_t kUnitInvincible = 0x50E; // byte, INVINCIBLE property; set on the pet

// CLevel destructor (non-deleting). It deletes every unit still in the owned list (+0x64).
constexpr uintptr_t kLevelDestructor = 0x004F9180;

constexpr int kAlignmentPlayer = 1;

template <typename T>
inline T field(const void* obj, uint32_t offset)
{
  return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(obj) + offset);
}

template <typename Fn>
inline Fn virtualFn(const void* obj, uint32_t index)
{
  auto vtbl = *reinterpret_cast<void* const* const*>(obj);
  return reinterpret_cast<Fn>(vtbl[index]);
}

// vtable[33] (+0x84), 0x423500: copies the 4x4 into +0xAC, orients the scene node, and
// moves the unit to the matrix translation only if setPosition is true.
using SetTransformFn = void(__thiscall*)(Unit* unit, const float* matrix4, bool setPosition);

inline void setTransform(Unit* unit, const float* matrix4, bool setPos)
{
  virtualFn<SetTransformFn>(unit, 33)(unit, matrix4, setPos);
}

inline void setPosition(Unit* unit, const Vec3& p)
{
  virtualFn<SetPositionFn>(unit, 10)(unit, p.x, p.y, p.z);
}

// The killer matters (0x4A9800): only with one does the game fire the "KILL" event (quests, level
// logic, at the unit's position) and roll the unit's treasure: monsters have a treasure table (+0x614)
// rolled by 0x5FF560 + CLevel 0x4F7790 (Kill's call returns to 0x4AACDB); without one, CLevel 0x4F7A40
// (returns to 0x4AAC6E; breakables use it too, from 0x48453D). Rolls often drop nothing.
// XP is not here: TakeDamage pays that.
constexpr uintptr_t kDropTreasureTable = 0x004F7790;
constexpr uintptr_t kDropTreasure = 0x004F7A40;
// The treasure table itself: Kill hands the unit's table (+0x614) to 0x5FF560, which resolves it via
// 0x5FF240 -> CSpawnClass 0x53ABB0 (thiscall(spawnClass, 10 args) ret 0x28): entries at +0x8 (count
// +0xC), each { data*, target* }, weight at data+0x8 (-1 = always). Positive weights go into a weighted
// bag (0x5BF910) and one entry is drawn. The target is a CSpawnClassData: its source text at +0x8
// (std::wstring, e.g. "UNITTYPE:NONE", "SPAWNCLASS:TREASURE"), sub-table +0x50, and +0x54 set for
// NONE (drawing it ends the pick with nothing).
// Data: media/Spawnclasses/*.DAT.adm (re/adm.py decodes them).
constexpr uintptr_t kSpawnClassPick = 0x0053ABB0;
constexpr uintptr_t kKillTableRollReturn = 0x004AACDB;
constexpr uintptr_t kKillDefaultRollReturn = 0x004AAC6E;
inline void kill(Unit* unit, Unit* killer = nullptr)
{
  virtualFn<KillFn>(unit, 90)(unit, static_cast<int>(reinterpret_cast<uintptr_t>(killer)), nullptr, 0.0f, 0);
}

// Ogre's operator new for AllocatedObject<CategorisedAllocPolicy<0>>, which the game's own
// `new` of units and save states uses (they are freed through the same allocator).
inline void* ogreNew(size_t size)
{
  using OgreNewFn = void*(__cdecl*)(size_t);
  static auto fn = reinterpret_cast<OgreNewFn>(GetProcAddress(
    GetModuleHandleW(L"OgreMain.dll"), "??2?$AllocatedObject@V?$CategorisedAllocPolicy@$0A@@Ogre@@@Ogre@@SAPAXI@Z"));
  return fn ? fn(size) : nullptr;
}

inline Unit* createMonsterByGuid(void* factory, uint64_t guid, int level)
{
  void* list = *reinterpret_cast<void**>(kUnitResourceList);
  if (!list || !factory)
    return nullptr;
  void* descriptor = reinterpret_cast<LookupGuidFn>(0x005321A0)(list, static_cast<uint32_t>(guid),
                                                                 static_cast<uint32_t>(guid >> 32));
  if (!descriptor)
    return nullptr;
  void* mem = ogreNew(kMonsterSize);
  if (!mem)
    return nullptr;
  Unit* unit = reinterpret_cast<MonsterCtorFn>(0x004D5C60)(mem, factory, level < 1 ? 1 : level);
  virtualFn<InitFromDescriptorFn>(unit, 61)(unit, descriptor, 0);
  return unit;
}

inline void destroyUnit(Unit* unit)
{
  virtualFn<DeleteFn>(unit, 0)(unit, 1);
}

} // namespace tlmp::engine
