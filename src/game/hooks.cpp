#include "game/hooks.h"
#include "game/anim.h"
#include "game/combat.h"
#include "game/floor.h"
#include "game/loot.h"
#include "game/quests.h"
#include "game/spawns.h"
#include "game/monsters.h"
#include "game/party.h"
#include "game/triggers.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/game.h"
#include "game/internal.h"
#include "log.h"

#include <MinHook.h>
#include <intrin.h>

#include <cstring>
#include <string>

namespace tlmp::hooks {

// CLevel::~CLevel is __thiscall(this). A __fastcall detour receives ecx as its first
// argument (edx is unused), which lets us intercept it without inline asm.
using LevelDtorFn = void(__fastcall*)(void* self, void* edx);
static LevelDtorFn g_levelDtor = nullptr;

static void __fastcall levelDtorHook(void* self, void* edx)
{
  game::floor::onLevelDestroyed(self); // first: its snapshot needs the level's monsters and seed
  game::monsters::onLevelDestroyed(self);
  game::loot::onLevelDestroyed(self);
  game::onLevelDestroyed(self);
  g_levelDtor(self, edx);
}

// CMonster::UpdateAI(dt, force) -- skipped for monsters the host is running.
using UpdateAIFn = void(__fastcall*)(void* self, void* edx, float dt, char force);
static UpdateAIFn g_updateAI = nullptr;

static void __fastcall updateAIHook(void* self, void* edx, float dt, char force)
{
  if (game::monsters::shouldSkipAI(self))
    return;
  g_updateAI(self, edx, dt, force);
}

// CCharacter::Kill(killer, dir, force, flag) -- refused for host-run monsters (until the host
// reports the death) and for remote players' avatars.
using KillFn = void(__fastcall*)(void* self, void* edx, int killer, const void* dir, float force, char flag);
static KillFn g_kill = nullptr;

static void __fastcall killHook(void* self, void* edx, int killer, const void* dir, float force, char flag)
{
  if (game::monsters::shouldBlockKill(self))
    return;
  g_kill(self, edx, game::combat::killerFor(self, killer), dir, force, flag);
}

// CCharacter::TakeDamage -- hits on monster copies go to the host, hits on stand-ins to their owner.
using TakeDamageHookFn = void(__fastcall*)(void* self, void* edx, void* level, const engine::Vec3* source,
                                           float damage, const engine::Vec3* dir, float force, void* attacker,
                                           char showNumber, char react, char knockback, int arg11, uint32_t flags);
static TakeDamageHookFn g_takeDamage = nullptr;

static void __fastcall takeDamageHook(void* self, void* edx, void* level, const engine::Vec3* source, float damage,
                                      const engine::Vec3* dir, float force, void* attacker, char showNumber,
                                      char react, char knockback, int arg11, uint32_t flags)
{
  game::combat::Hit hit{level, source, damage, dir, force, attacker, showNumber, react, knockback, arg11, flags};
  if (!game::combat::beforeTakeDamage(self, hit))
    return;
  g_takeDamage(self, edx, level, source, hit.damage, dir, force, attacker, showNumber, react, knockback, arg11, flags);
}

// CPlayer::Update runs for stand-ins too; the death screen and tips it may open are only the
// local player's (see engine.h, "death screen").
using PlayerUpdateFn = void(__fastcall*)(void* self, void* edx, uint32_t a, uint32_t b, float dt);
static PlayerUpdateFn g_playerUpdate = nullptr;

static void __fastcall playerUpdateHook(void* self, void* edx, uint32_t a, uint32_t b, float dt)
{
  game::setUpdatingPlayer(self);
  g_playerUpdate(self, edx, a, b, dt);
  game::setUpdatingPlayer(nullptr);
}

using ShowDieMenuFn = void(__fastcall*)(void* self, void* edx);
static ShowDieMenuFn g_showDieMenu = nullptr;

static void __fastcall showDieMenuHook(void* self, void* edx)
{
  if (game::allowPlayerUi())
    g_showDieMenu(self, edx);
}

using ShowTipFn = void(__fastcall*)(void* self, void* edx, int id);
static ShowTipFn g_showTip = nullptr;

static void __fastcall showTipHook(void* self, void* edx, int id)
{
  if (game::allowPlayerUi())
    g_showTip(self, edx, id);
}

// CCharacter::GiveExperience / GiveFame(level, victim, amount, raw).
using GiveRewardHookFn = void(__fastcall*)(void* self, void* edx, void* level, void* victim, int amount, char raw);
static GiveRewardHookFn g_giveExperience = nullptr;
static GiveRewardHookFn g_giveFame = nullptr;

static void __fastcall giveExperienceHook(void* self, void* edx, void* level, void* victim, int amount, char raw)
{
  if (game::combat::allowReward(self, level, victim, amount, raw, false))
    g_giveExperience(self, edx, level, victim, amount, raw);
}

static void __fastcall giveFameHook(void* self, void* edx, void* level, void* victim, int amount, char raw)
{
  if (game::combat::allowReward(self, level, victim, amount, raw, true))
    g_giveFame(self, edx, level, victim, amount, raw);
}

// Animation object calls (engine.h, "animation"): play / crossfade / queue by index. Every
// argument is a 4-byte stack slot, passed through untouched.
using AnimCall4Fn = void(__fastcall*)(void* self, void* edx, uint32_t index, uint32_t a, uint32_t b, uint32_t c);
using AnimCall5Fn = void(__fastcall*)(void* self, void* edx, uint32_t index, uint32_t a, uint32_t b, uint32_t c,
                                      uint32_t d);
static AnimCall4Fn g_animPlay = nullptr;
static AnimCall5Fn g_animCrossfade = nullptr;
static AnimCall4Fn g_animQueue = nullptr;

static void __fastcall animPlayHook(void* self, void* edx, uint32_t index, uint32_t a, uint32_t b, uint32_t c)
{
  const uint32_t args[] = {a, b, c};
  game::anim::onCall(self, game::anim::Call::Play, index, args, 3);
  g_animPlay(self, edx, index, a, b, c);
}

static void __fastcall animCrossfadeHook(void* self, void* edx, uint32_t index, uint32_t a, uint32_t b, uint32_t c,
                                         uint32_t d)
{
  const uint32_t args[] = {a, b, c, d};
  game::anim::onCall(self, game::anim::Call::Crossfade, index, args, 4);
  g_animCrossfade(self, edx, index, a, b, c, d);
}

static void __fastcall animQueueHook(void* self, void* edx, uint32_t index, uint32_t a, uint32_t b, uint32_t c)
{
  const uint32_t args[] = {a, b, c};
  game::anim::onCall(self, game::anim::Call::Queue, index, args, 3);
  g_animQueue(self, edx, index, a, b, c);
}

// Trigger volumes: one inside/outside decision per update for the whole party (spawns.h).
using TriggerVolumeUpdateFn = void(__fastcall*)(void* self, void* edx, uint32_t a, uint32_t b);
using TriggerVolumeHandlerFn = void(__fastcall*)(void* self, void* edx, void* player);
static TriggerVolumeUpdateFn g_triggerBoxUpdate = nullptr;
static TriggerVolumeUpdateFn g_triggerSphereUpdate = nullptr;
static TriggerVolumeHandlerFn g_triggerInside = nullptr;
static TriggerVolumeHandlerFn g_triggerOutside = nullptr;

static void runTriggerUpdate(TriggerVolumeUpdateFn original, void* self, void* edx, uint32_t a, uint32_t b)
{
  game::spawns::beginTriggerUpdate(self);
  original(self, edx, a, b);
  game::spawns::endTriggerUpdate(self, reinterpret_cast<game::spawns::HandlerFn>(g_triggerInside),
                                 reinterpret_cast<game::spawns::HandlerFn>(g_triggerOutside));
}

static void __fastcall triggerBoxUpdateHook(void* self, void* edx, uint32_t a, uint32_t b)
{
  runTriggerUpdate(g_triggerBoxUpdate, self, edx, a, b);
}

static void __fastcall triggerSphereUpdateHook(void* self, void* edx, uint32_t a, uint32_t b)
{
  runTriggerUpdate(g_triggerSphereUpdate, self, edx, a, b);
}

static void __fastcall triggerInsideHook(void* self, void* edx, void* player)
{
  if (game::spawns::onTriggerInside(self, player))
    g_triggerInside(self, edx, player);
}

static void __fastcall triggerOutsideHook(void* self, void* edx, void* player)
{
  if (game::spawns::onTriggerOutside(self, player))
    g_triggerOutside(self, edx, player);
}

// Unit activity (who thinks and moves): near any player on a floor this machine runs (spawns.h).
using ActivityHookFn = void(__fastcall*)(void* self, void* edx, const void* pos);
static ActivityHookFn g_activity = nullptr;

static void __fastcall activityHook(void* self, void* edx, const void* pos)
{
  g_activity(self, edx, pos);
  game::spawns::afterActivityCheck(self, reinterpret_cast<game::spawns::ActivityFn>(g_activity));
}

// CQuestUnitData::Spawn(resources, pos) -> unit: quest monsters on a floor run elsewhere go to its owner.
using QuestSpawnHookFn = void*(__fastcall*)(void* self, void* edx, void* resources, const void* pos);
static QuestSpawnHookFn g_questSpawn = nullptr;

static void* __fastcall questSpawnHook(void* self, void* edx, void* resources, const void* pos)
{
  (void)edx;
  return game::quests::spawn(reinterpret_cast<game::quests::SpawnFn>(g_questSpawn), self, resources, pos);
}

// CLevel::AddItem(item, pos, flag) -> item: items the local player drops are shared.
using AddItemHookFn = void*(__fastcall*)(void* self, void* edx, void* item, const void* pos, char flag);
static AddItemHookFn g_addItem = nullptr;

static void* __fastcall addItemHook(void* self, void* edx, void* item, const void* pos, char flag)
{
  bool drop = game::loot::beforeAddItem(self, item, reinterpret_cast<uintptr_t>(_ReturnAddress()));
  void* result = g_addItem(self, edx, item, pos, flag);
  game::loot::afterAddItem(self, item, drop);
  return result;
}

// CCharacter::PickUp(item, level): shared items are the host's call.
using PickUpHookFn = uint32_t(__fastcall*)(void* self, void* edx, void* item, void* level);
static PickUpHookFn g_pickUp = nullptr;

static uint32_t __fastcall pickUpHook(void* self, void* edx, void* item, void* level)
{
  if (!game::loot::allowPickUp(self, item, level))
    return 0;
  return g_pickUp(self, edx, item, level);
}

// CTriggerUnit::Interact(user) -- doors/levers; reports state changes made by the local player.
using InteractFn = uint32_t(__fastcall*)(void* self, void* edx, void* user);
static InteractFn g_interact = nullptr;

static uint32_t __fastcall interactHook(void* self, void* edx, void* user)
{
  if (user && game::isAvatarUnit(user) && !engine::field<void*>(user, engine::kPlayerQuestManager))
    return 0; // stand-ins have no quest manager (see playerLinkQuestsHook); remote uses come as Activate
  int before = game::triggers::beforeInteract(self, user);
  uint32_t result = g_interact(self, edx, user);
  game::triggers::afterInteract(self, user, before);
  return result;
}

// CGameClient::ChangeLevel(wstring dungeon, int mode, int floor, char waypoint, wstring warp, char flag)
// with both strings by value (callee-destroyed). Party travel decides whether it runs now.
using ChangeLevelHookFn = void(__fastcall*)(void* self, void* edx, vs::WString dungeon, int mode, int floor,
                                            char waypoint, vs::WString warp, char flag);
static ChangeLevelHookFn g_changeLevel = nullptr;

static std::wstring readVsString(const vs::WString& s)
{
  wchar_t buf[256];
  return diag::readWStringAny(reinterpret_cast<uintptr_t>(&s), buf, 256) ? std::wstring(buf) : std::wstring();
}

static void __fastcall changeLevelHook(void* self, void* edx, vs::WString dungeon, int mode, int floor, char waypoint,
                                       vs::WString warp, char flag)
{
  game::party::TravelArgs args{readVsString(dungeon), mode, floor, waypoint, readVsString(warp), flag};
  bool allow = game::party::allowChangeLevel(self, args);
  TLMP_LOG("ChangeLevel('%ls', mode %d, floor %d, waypoint %d, warp '%ls', flag %d) on %p: %s", args.dungeon.c_str(),
           mode, floor, waypoint, args.warpName.c_str(), flag, self, allow ? "allowed" : "held for the party");
  if (allow)
  {
    g_changeLevel(self, edx, dungeon, mode, floor, waypoint, warp, flag);
    return;
  }
  // Refused: we own the by-value strings, so free them as the callee would have.
  vs::destroy(&dungeon);
  vs::destroy(&warp);
}

using EnterGameFn = void(__fastcall*)(void* self, void* edx, int a, int b);
static EnterGameFn g_enterGame = nullptr;

static void __fastcall enterGameHook(void* self, void* edx, int a, int b)
{
  uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
  // a == 5 is the resurrect pass (the die menu's choice, GameUI+0x16A8): not a new arrival.
  bool fromContinue = from != engine::kEnterGameAtStartup && a != engine::kEnterGameResurrect;
  TLMP_LOG("game: entering the game (from %08x, %d)%s", static_cast<unsigned>(from), a,
           from == engine::kEnterGameAtStartup ? " -- title screen" : a == engine::kEnterGameResurrect ? " -- resurrecting" : "");
  game::party::beginEnterGame();
  g_enterGame(self, edx, a, b);
  game::party::endEnterGame(fromContinue);
}

// CLevel::AddUnit(unit, pos, flag): clients refuse their own hostile monsters.
using AddUnitHookFn = void*(__fastcall*)(void* self, void* edx, void* unit, const void* pos, char flag);
static AddUnitHookFn g_addUnit = nullptr;

static void* __fastcall addUnitHook(void* self, void* edx, void* unit, const void* pos, char flag)
{
  if (!game::monsters::allowAddUnit(self, unit, reinterpret_cast<uintptr_t>(_ReturnAddress())))
    return unit; // never enters the level: not updated, not owned (or freed) by it
  return g_addUnit(self, edx, unit, pos, flag);
}

// CTriggerUnit::Activate(user): on clients the local player's uses go to the host.
using ActivateHookFn = void(__fastcall*)(void* self, void* edx, void* user);
static ActivateHookFn g_activate = nullptr;

static void __fastcall activateHook(void* self, void* edx, void* user)
{
  if (game::triggers::allowActivate(self, user))
    g_activate(self, edx, user);
}

// CPlayer quest link (player, manager): stand-ins for remote players must not take over the local
// player's quest manager -- linking resets it (engine.h, "quests").
using PlayerLinkQuestsFn = void(__fastcall*)(void* self, void* edx, void* manager);
static PlayerLinkQuestsFn g_playerLinkQuests = nullptr;

static void __fastcall playerLinkQuestsHook(void* self, void* edx, void* manager)
{
  if (manager && game::creatingStandIn())
  {
    // Unlinked, its destructor leaves the manager alone too.
    *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(self) + engine::kPlayerQuestManager) = nullptr;
    TLMP_LOG("quests: stand-in %p kept off the quest manager", self);
    return;
  }
  g_playerLinkQuests(self, edx, manager);
}

// CTriggerUnit init(descriptor, arg) and destructor: track live doors/levers/plungers.
using TriggerInitFn = void(__fastcall*)(void* self, void* edx, void* descriptor, int arg);
static TriggerInitFn g_triggerInit = nullptr;

static void __fastcall triggerInitHook(void* self, void* edx, void* descriptor, int arg)
{
  g_triggerInit(self, edx, descriptor, arg);
  game::triggers::onTriggerCreated(self);
}

using TriggerDtorFn = void(__fastcall*)(void* self, void* edx);
static TriggerDtorFn g_triggerDtor = nullptr;

static void __fastcall triggerDtorHook(void* self, void* edx)
{
  game::triggers::onTriggerDestroyed(self);
  g_triggerDtor(self, edx);
}

using SaveCharacterFn = void(__fastcall*)(void* client, void* edx, bool a, bool b);
static SaveCharacterFn g_saveCharacter = nullptr;

static void __fastcall saveCharacterHook(void* client, void* edx, bool a, bool b)
{
  TLMP_LOG("save: game is saving the character (from %08x) on level %08x", static_cast<unsigned>(reinterpret_cast<uintptr_t>(_ReturnAddress())),
           game::currentLevelId());
  game::monsters::beforeSave();
  game::loot::beforeSave();
  game::beforeSave(client);
  g_saveCharacter(client, edx, a, b);
  game::afterSave(client);
}

// RNG seed (0x5BFD70, cdecl void(DWORD seed)). At the layout seeding call (0x4FF6DC) we
// record which seed each level was built from and, in a session, substitute the host's
// world seed. A naked stub captures ESI
// first, because at that call site ESI holds the CLevel* being built.
using SeedRngFn = void(__cdecl*)(uint32_t seed);
static SeedRngFn g_seedRng = nullptr;
static uint32_t g_seedCallerEsi = 0;

static void __cdecl seedRngHook(uint32_t seed)
{
  if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == engine::kLayoutSeedReturn)
    seed = game::onLevelLayoutSeeded(reinterpret_cast<void*>(g_seedCallerEsi), seed);
  g_seedRng(seed);
}

static __declspec(naked) void seedRngStub()
{
  __asm {
    mov g_seedCallerEsi, esi
    jmp seedRngHook
  }
}

// Level choice when a character loads (diagnostics for a rare start on the wrong floor). The loader
// 0x415A50 (thiscall, 0x26C frame) picks the floor's layout descriptor -- CDungeon (client+0x242C)
// ->0x4EDB50(client, floor) after seeding with 0x40D250 -- only when its stack arguments +0x14/+0x30
// allow and +0x38 is clear; without a descriptor it builds the level from a file (0x500BC0).
static void* g_loadCharacter = nullptr;
static void* g_levelFromFile = nullptr;

static void __stdcall logLoadCharacter(uintptr_t client, const uint32_t* args)
{
  uint32_t dungeon = 0;
  diag::readU32(client + 0x242C, dungeon);
  TLMP_LOG("level choice: loading the character (from %08x): dungeon %08x, args +14 %08x +30 %08x +38 %02x floor %d",
           args[-1], dungeon, args[0x14 / 4], args[0x30 / 4], args[0x38 / 4] & 0xFF, static_cast<int>(args[0x3C / 4]));
}

static void __stdcall logLevelFromFile(void* level, const uint32_t* args)
{
  wchar_t path[160] = {};
  diag::readWString(reinterpret_cast<uintptr_t>(args), path, 160);
  // args: wstring path (+0), char restore (+0x1C): set = the file's saved level as is (0x4FF120 gets no
  // descriptor and skips generation: no layout seed), clear = generated from the file's rules.
  TLMP_LOG("level choice: level %p from file '%ls' (from %08x), %s", level, path, args[-1],
           (args[0x1C / 4] & 0xFF) ? "restored as saved -- no layout seed" : "generated from its rules");
}

static __declspec(naked) void loadCharacterStub()
{
  __asm {
    pushad
    lea eax, [esp + 36]
    push eax
    push ecx
    call logLoadCharacter
    popad
    jmp g_loadCharacter
  }
}

static __declspec(naked) void levelFromFileStub()
{
  __asm {
    pushad
    lea eax, [esp + 36]
    push eax
    push ecx
    call logLevelFromFile
    popad
    jmp g_levelFromFile
  }
}

// Treasure rolls (CLevel 0x4F7790 / 0x4F7A40, engine.h): counts Kill's rolls for the rig; what a roll
// drops is random.
static void* g_dropTreasureTable = nullptr;
static void* g_dropTreasure = nullptr;
static volatile unsigned g_killTreasureRolls = 0;

static void __stdcall onTreasureRoll(uint32_t from)
{
  if (from == engine::kKillTableRollReturn || from == engine::kKillDefaultRollReturn)
    g_killTreasureRolls++;
}

static __declspec(naked) void dropTreasureTableStub()
{
  __asm {
    pushad
    push dword ptr [esp + 32]
    call onTreasureRoll
    popad
    jmp g_dropTreasureTable
  }
}

static __declspec(naked) void dropTreasureStub()
{
  __asm {
    pushad
    push dword ptr [esp + 32]
    call onTreasureRoll
    popad
    jmp g_dropTreasure
  }
}

unsigned killTreasureRolls()
{
  return g_killTreasureRolls;
}

using PropertyFileWriteFn = bool(__fastcall*)(void* self, void* edx, void* path, int flag);
static PropertyFileWriteFn g_propertyFileWrite = nullptr;

static bool __fastcall propertyFileWriteHook(void* self, void* edx, void* path, int flag)
{
  game::beforeSettingsWrite(self);
  bool ok = g_propertyFileWrite(self, edx, path, flag);
  game::afterSettingsWrite(self);
  return ok;
}

// CConsole::Execute (0x56CD00) is __thiscall(console, std::wstring line) with the 28-byte
// VS2008 string passed by value at [esp+4] and popped by the callee (ret 0x1C). The stub
// shows the line to game::onConsoleLine; if that consumes it (chat), the stub destroys the
// by-value string itself and returns as the original would, otherwise it tail-jumps on.
static void* g_consoleExecute = nullptr;
static void* g_wstringDtor = nullptr;

static bool __cdecl consoleFilter(const void* vsWString)
{
  wchar_t line[256];
  if (!diag::readWStringAny(reinterpret_cast<uintptr_t>(vsWString), line, 256))
    return false;
  return game::onConsoleLine(line);
}

static __declspec(naked) void consoleExecuteStub()
{
  __asm {
    push ecx
    lea eax, [esp + 8]
    push eax
    call consoleFilter
    add esp, 4
    pop ecx
    test al, al
    jnz consumed
    jmp dword ptr [g_consoleExecute]
  consumed:
    lea ecx, [esp + 4]
    call dword ptr [g_wstringDtor]
    ret 0x1c
  }
}

// Replaces `expected` at `at` with `replacement` (same length) if the code is what we expect.
static bool patchCode(uintptr_t at, const uint8_t* expected, const uint8_t* replacement, size_t size, const char* name)
{
  auto* code = reinterpret_cast<uint8_t*>(at);
  if (std::memcmp(code, expected, size) != 0)
  {
    TLMP_LOG("hooks: %s: unexpected code at %08x, left alone", name, static_cast<unsigned>(at));
    return false;
  }
  DWORD old = 0;
  if (!VirtualProtect(code, size, PAGE_EXECUTE_READWRITE, &old))
    return false;
  std::memcpy(code, replacement, size);
  VirtualProtect(code, size, old, &old);
  FlushInstructionCache(GetCurrentProcess(), code, size);
  TLMP_LOG("hooks: patched %s", name);
  return true;
}

static bool hook(uintptr_t target, void* detour, void** original, const char* name)
{
  MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
  if (st != MH_OK)
  {
    TLMP_LOG("hooks: MH_CreateHook(%s @%08x) failed (%d)", name, static_cast<unsigned>(target), st);
    return false;
  }
  return true;
}

bool install()
{
  MH_STATUS st = MH_Initialize();
  if (st != MH_OK)
  {
    TLMP_LOG("hooks: MH_Initialize failed (%d)", st);
    return false;
  }

  if (!hook(engine::kLevelDestructor, reinterpret_cast<void*>(&levelDtorHook),
            reinterpret_cast<void**>(&g_levelDtor), "CLevel::~CLevel"))
    return false;
  if (!hook(engine::kSaveCharacter, reinterpret_cast<void*>(&saveCharacterHook),
            reinterpret_cast<void**>(&g_saveCharacter), "SaveCharacter"))
    return false;
  g_wstringDtor = GetProcAddress(GetModuleHandleW(L"msvcp90.dll"),
                                 "??1?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAE@XZ");
  if (!g_wstringDtor ||
      !hook(engine::kConsoleExecute, reinterpret_cast<void*>(&consoleExecuteStub), &g_consoleExecute, "ConsoleExecute"))
    return false;
  if (!hook(engine::kPropertyFileWrite, reinterpret_cast<void*>(&propertyFileWriteHook),
            reinterpret_cast<void**>(&g_propertyFileWrite), "PropertyFileWrite"))
    return false;
  if (!hook(engine::kMonsterUpdateAI, reinterpret_cast<void*>(&updateAIHook), reinterpret_cast<void**>(&g_updateAI),
            "CMonster::UpdateAI") ||
      !hook(engine::kCharacterKill, reinterpret_cast<void*>(&killHook), reinterpret_cast<void**>(&g_kill),
            "CCharacter::Kill"))
    return false;
  if (!hook(engine::kTakeDamage, reinterpret_cast<void*>(&takeDamageHook), reinterpret_cast<void**>(&g_takeDamage),
            "CCharacter::TakeDamage") ||
      !hook(engine::kGiveExperience, reinterpret_cast<void*>(&giveExperienceHook),
            reinterpret_cast<void**>(&g_giveExperience), "CCharacter::GiveExperience") ||
      !hook(engine::kGiveFame, reinterpret_cast<void*>(&giveFameHook), reinterpret_cast<void**>(&g_giveFame),
            "CCharacter::GiveFame"))
    return false;
  if (!hook(engine::kPlayerUpdate, reinterpret_cast<void*>(&playerUpdateHook),
            reinterpret_cast<void**>(&g_playerUpdate), "CPlayer::Update") ||
      !hook(engine::kShowDieMenu, reinterpret_cast<void*>(&showDieMenuHook), reinterpret_cast<void**>(&g_showDieMenu),
            "CGameUI::ShowDieMenu") ||
      !hook(engine::kShowTip, reinterpret_cast<void*>(&showTipHook), reinterpret_cast<void**>(&g_showTip),
            "ShowTip"))
    return false;
  if (!hook(engine::kAnimPlay, reinterpret_cast<void*>(&animPlayHook), reinterpret_cast<void**>(&g_animPlay),
            "Animation::Play") ||
      !hook(engine::kAnimCrossfade, reinterpret_cast<void*>(&animCrossfadeHook),
            reinterpret_cast<void**>(&g_animCrossfade), "Animation::Crossfade") ||
      !hook(engine::kAnimQueue, reinterpret_cast<void*>(&animQueueHook), reinterpret_cast<void**>(&g_animQueue),
            "Animation::Queue"))
    return false;
  if (!hook(engine::kTriggerBoxUpdate, reinterpret_cast<void*>(&triggerBoxUpdateHook),
            reinterpret_cast<void**>(&g_triggerBoxUpdate), "CTriggerBox::Update") ||
      !hook(engine::kTriggerSphereUpdate, reinterpret_cast<void*>(&triggerSphereUpdateHook),
            reinterpret_cast<void**>(&g_triggerSphereUpdate), "CTriggerSphere::Update") ||
      !hook(engine::kTriggerVolumeInside, reinterpret_cast<void*>(&triggerInsideHook),
            reinterpret_cast<void**>(&g_triggerInside), "CLogicTrigger::Inside") ||
      !hook(engine::kTriggerVolumeOutside, reinterpret_cast<void*>(&triggerOutsideHook),
            reinterpret_cast<void**>(&g_triggerOutside), "CLogicTrigger::Outside") ||
      !hook(engine::kUnitActivity, reinterpret_cast<void*>(&activityHook), reinterpret_cast<void**>(&g_activity),
            "CCharacter::UpdateActivity"))
    return false;
  if (!hook(engine::kPlayerLinkQuests, reinterpret_cast<void*>(&playerLinkQuestsHook),
            reinterpret_cast<void**>(&g_playerLinkQuests), "CPlayer::LinkQuests"))
    return false;
  if (!hook(engine::kQuestUnitSpawn, reinterpret_cast<void*>(&questSpawnHook), reinterpret_cast<void**>(&g_questSpawn),
            "CQuestUnitData::Spawn"))
    return false;
  if (!hook(engine::kLevelAddItem, reinterpret_cast<void*>(&addItemHook), reinterpret_cast<void**>(&g_addItem),
            "CLevel::AddItem") ||
      !hook(engine::kPickUp, reinterpret_cast<void*>(&pickUpHook), reinterpret_cast<void**>(&g_pickUp),
            "CCharacter::PickUp"))
    return false;
  if (!hook(engine::kTriggerInteract, reinterpret_cast<void*>(&interactHook), reinterpret_cast<void**>(&g_interact),
            "CTriggerUnit::Interact"))
    return false;
  if (!hook(engine::kAddUnit, reinterpret_cast<void*>(&addUnitHook), reinterpret_cast<void**>(&g_addUnit),
            "CLevel::AddUnit") ||
      !hook(engine::kTriggerActivate, reinterpret_cast<void*>(&activateHook), reinterpret_cast<void**>(&g_activate),
            "CTriggerUnit::Activate"))
    return false;
  if (!hook(engine::kTriggerInit, reinterpret_cast<void*>(&triggerInitHook), reinterpret_cast<void**>(&g_triggerInit),
            "CTriggerUnit::Init") ||
      !hook(engine::kTriggerDtor, reinterpret_cast<void*>(&triggerDtorHook), reinterpret_cast<void**>(&g_triggerDtor),
            "CTriggerUnit::~CTriggerUnit"))
    return false;
  if (!hook(engine::kEnterGame, reinterpret_cast<void*>(&enterGameHook), reinterpret_cast<void**>(&g_enterGame),
            "EnterGame"))
    return false;
  if (!hook(engine::kChangeLevel, reinterpret_cast<void*>(&changeLevelHook), reinterpret_cast<void**>(&g_changeLevel),
            "CGameClient::ChangeLevel"))
    return false;
  if (!hook(engine::kSeedRng, reinterpret_cast<void*>(&seedRngStub), reinterpret_cast<void**>(&g_seedRng),
            "SeedRng"))
    return false;
  if (!hook(0x00415A50, reinterpret_cast<void*>(&loadCharacterStub), &g_loadCharacter, "LoadCharacter (log)") ||
      !hook(0x00500BC0, reinterpret_cast<void*>(&levelFromFileStub), &g_levelFromFile, "CLevel::BuildFromFile (log)"))
    return false;
  if (!hook(engine::kDropTreasureTable, reinterpret_cast<void*>(&dropTreasureTableStub), &g_dropTreasureTable,
            "CLevel::DropTreasureTable") ||
      !hook(engine::kDropTreasure, reinterpret_cast<void*>(&dropTreasureStub), &g_dropTreasure, "CLevel::DropTreasure"))
    return false;
  {
    // Shift + Continue must not send one player to the developer test room (engine.h).
    static const uint8_t je[] = {0x0F, 0x84, 0x5F, 0x01, 0x00, 0x00};
    static const uint8_t jmp[] = {0xE9, 0x60, 0x01, 0x00, 0x00, 0x90};
    patchCode(engine::kTestRoomShiftCheck, je, jmp, sizeof(je), "Shift+Continue test room");
  }

  st = MH_EnableHook(MH_ALL_HOOKS);
  if (st != MH_OK)
  {
    TLMP_LOG("hooks: MH_EnableHook failed (%d)", st);
    return false;
  }
  return true;
}

void uninstall()
{
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();
}

} // namespace tlmp::hooks
