#include "game/game.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/internal.h"
#include "game/autotest.h"
#include "game/floor.h"
#include "game/party.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tlmp::game {

using engine::Vec3;

namespace ogre {
struct Quaternion
{
  float w, x, y, z;
};
using GetDerivedOrientationFn = const Quaternion&(__thiscall*)(const void* node);
} // namespace ogre

static ogre::GetDerivedOrientationFn g_getDerivedOrientation = nullptr;


// Order-independent hash of every scene node's world position (rounded to 1 unit),
// starting from the root above the local player's node. Static room geometry dominates
// it, so equal values mean the same floor layout. Diagnostic only.
static uint32_t sceneFingerprint(uint32_t* nodeCount)
{
  using NumChildrenFn = unsigned short(__thiscall*)(const void*);
  using GetChildFn = void*(__thiscall*)(const void*, unsigned short);
  using GetParentFn = void*(__thiscall*)(const void*);
  using GetPosFn = const Vec3&(__thiscall*)(const void*);
  HMODULE ogreMain = GetModuleHandleW(L"OgreMain.dll");
  auto numChildren = reinterpret_cast<NumChildrenFn>(GetProcAddress(ogreMain, "?numChildren@Node@Ogre@@UBEGXZ"));
  auto getChild = reinterpret_cast<GetChildFn>(GetProcAddress(ogreMain, "?getChild@Node@Ogre@@UBEPAV12@G@Z"));
  auto getParent = reinterpret_cast<GetParentFn>(GetProcAddress(ogreMain, "?getParent@Node@Ogre@@UBEPAV12@XZ"));
  auto getPos = reinterpret_cast<GetPosFn>(GetProcAddress(ogreMain, "?_getDerivedPosition@Node@Ogre@@UBEABVVector3@2@XZ"));
  uint32_t player = static_cast<uint32_t>(localPlayer()), node = 0;
  if (!numChildren || !getChild || !getParent || !getPos || !player || !diag::readU32(player + engine::kUnitNode, node) || !node)
    return 0;
  void* root = reinterpret_cast<void*>(node);
  while (void* parent = getParent(root))
    root = parent;

  // Skip the subtrees of every unit in the level (characters, pets, monsters, props with
  // AI): their nodes animate or depend on the character, not on the layout.
  std::vector<void*> skip;
  if (engine::Level* lvl = currentLevel())
  {
    for (uint32_t listOff : {0x5Cu, 0x64u, 0x80u})
    {
      uint32_t list = 0, n = 0;
      if (!diag::readU32(reinterpret_cast<uintptr_t>(lvl) + listOff, list) || !list || !diag::readU32(list, n))
        continue;
      for (uint32_t guard = 0; n && guard < 20000; guard++)
      {
        uint32_t unit = 0, next = 0, unode = 0;
        diag::readU32(n, unit);
        diag::readU32(n + 4, next);
        if (diag::readU32(unit + engine::kUnitNode, unode) && unode)
          skip.push_back(reinterpret_cast<void*>(unode));
        n = next;
      }
    }
  }
  const Vec3 me = engine::field<Vec3>(reinterpret_cast<void*>(player), engine::kUnitPosition);
  skip.push_back(reinterpret_cast<void*>(node));
  if (uintptr_t pet = localPet(); pet && diag::readU32(pet + engine::kUnitNode, node) && node)
    skip.push_back(reinterpret_cast<void*>(node));

  uint32_t sum = 0, count = 0;
  std::vector<void*> stack{root};
  while (!stack.empty() && count < 200000)
  {
    void* n = stack.back();
    stack.pop_back();
    if (std::find(skip.begin(), skip.end(), n) != skip.end())
      continue;
    const Vec3& p = getPos(n);
    unsigned short kidsHere = numChildren(n);
    for (unsigned short i = 0; i < kidsHere; i++)
      stack.push_back(getChild(n, i));
    // Character-attached effects aren't always parented to the unit node; ignore anything
    // near where the local character stands.
    if (distXZ(p, me) < 5.0f)
      continue;
    uint32_t h = 2166136261u;
    for (float f : {p.x, p.y, p.z})
      h = (h ^ static_cast<uint32_t>(static_cast<int32_t>(std::floor(f)))) * 16777619u;
    sum += h;
    count++;
  }
  *nodeCount = count;
  return sum;
}
static float g_clock = 0.0f;

float clock()
{
  return g_clock;
}
static uintptr_t g_lastLocalPlayer = 0;

// --- remote avatars ------------------------------------------------------------------
// A remote player is shown as real game units created through the game's own factories
// (a CPlayer and, if they have one, their pet CMonster), registered with the local level
// and steered with the click-to-move function so they path and animate normally.

struct Avatar
{
  net::PlayerInfo info;
  net::PlayerState state;
  bool hasState = false;

  Puppet player;
  Puppet pet;
  std::vector<void*> removedItems; // default gear taken off the avatar; freed with it
  uint8_t* clientEntry = nullptr;  // host: stand-in CGameClient registered for this player
  uint64_t petFailedGuid = 0;      // pet GUID that could not be created (not retried)
  bool wasDead = false;
};

static std::array<Avatar, net::kMaxPlayers> g_avatars;

static void registerClientEntry(Avatar& a, uint8_t slot);
static bool standInsCountHere();
static void cleanMinions();
static void unregisterClientEntry(Avatar& a);
static void refreshClientEntry(Avatar& a);

// --- shared world seed ---------------------------------------------------------------
// Floor layouts are seeded from client+0xFB0 (a per-character world seed loaded from the
// save) plus values that are identical for everyone. Clients write the host's value over
// their own while connected so every floor generates the same; their own value is put back
// when the session ends, so the next save keeps the character's original world.
static uint32_t g_hostWorldSeed = 0; // 0 = not overriding
static uint32_t g_ownWorldSeed = 0;  // the value we replaced

static void applyHostWorldSeed();

// Seed each live level's layout was built from (recorded by the RNG hook).
static std::unordered_map<void*, uint32_t> g_layoutSeeds;

// --- local state -----------------------------------------------------------------------

uintptr_t gameClient()
{
  uint32_t game, client;
  if (!diag::readU32(engine::kGameGlobal, game) || !game)
    return 0;
  if (!diag::readU32(game + engine::kGameClient, client) || !client)
    return 0;
  return client;
}

static uintptr_t clientObject(uint32_t offset, const char* rtti)
{
  uintptr_t client = gameClient();
  uint32_t obj;
  if (!client || !diag::readU32(client + offset, obj))
    return 0;
  const char* n = diag::rttiName(obj);
  return n && std::strcmp(n, rtti) == 0 ? obj : 0;
}

uintptr_t localPlayer()
{
  return clientObject(engine::kClientPlayer, ".?AVCPlayer@@");
}

uintptr_t localPet()
{
  return clientObject(engine::kClientPet, ".?AVCMonster@@");
}

engine::Level* currentLevel()
{
  return reinterpret_cast<engine::Level*>(clientObject(engine::kClientLevel, ".?AVCLevel@@"));
}

// True when there is a local character standing in a level. Deliberately not tied to
// the title screen: "Continue" keeps the title screen's TOWN level and character and just
// hands over control, so the title scene is already the real gameplay level, and no field
// found so far distinguishes the two (client+0xF80 stays 0 in both).
static bool inGameplay()
{
  // The title screen is a real level with the character standing in it: not "in the game" until
  // the player has pressed Continue.
  return localPlayer() != 0 && currentLevel() != nullptr && party::enteredFromContinue();
}

// Identifies "the same level, built the same way". Mixing in the layout seed keeps
// avatars hidden while two players stand on same-named floors with different layouts
// (e.g. the floor a client was already on before adopting the host's world seed). Town's
// layout is fixed, so its seed is ignored there.
uint32_t levelId(engine::Level* level)
{
  wchar_t name[128];
  if (!level || !diag::readWString(reinterpret_cast<uintptr_t>(level) + engine::kLevelName, name, 128))
    return 0;
  uint32_t h = 2166136261u; // FNV-1a
  for (const wchar_t* p = name; *p; p++)
    h = (h ^ static_cast<uint32_t>(towupper(*p))) * 16777619u;
  if (_wcsicmp(name, L"TOWN") != 0)
  {
    auto it = g_layoutSeeds.find(level);
    uint32_t seed = it != g_layoutSeeds.end() ? it->second : 0;
    h = (h ^ seed) * 16777619u;
  }
  return h;
}

Vec3 unitPosition(engine::Unit* unit)
{
  return engine::field<Vec3>(unit, engine::kUnitPosition);
}

float distXZ(const Vec3& a, const Vec3& b)
{
  float dx = a.x - b.x, dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

static std::string readAscii(uintptr_t wstringAddr)
{
  wchar_t buf[64];
  if (!wstringAddr || !diag::readWString(wstringAddr, buf, 64))
    return {};
  std::string out;
  for (const wchar_t* p = buf; *p; p++)
    out.push_back(static_cast<char>(*p));
  return out;
}

std::string toUtf8(const wchar_t* w)
{
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1)
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring fromUtf8(const std::string& s)
{
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1)
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  return out;
}

// Display names may contain any character (sent as UTF-8).
static std::string readName(uintptr_t wstringAddr)
{
  wchar_t buf[64];
  if (!wstringAddr || !diag::readWStringAny(wstringAddr, buf, 64))
    return {};
  return toUtf8(buf);
}

net::PlayerInfo localPlayerInfo()
{
  net::PlayerInfo info;
  if (uintptr_t player = localPlayer())
  {
    info.className = readAscii(player + engine::kPlayerClassName);
    info.charName = readName(player + engine::kUnitName);
    void* inv = engine::field<void*>(reinterpret_cast<void*>(player), engine::kUnitInventory);
    for (int slot = 0; inv && slot < net::kEquipSlots; slot++)
    {
      if (void* item = engine::GetItemInSlot(inv, slot))
        info.equipment[slot] = engine::field<uint64_t>(item, engine::kUnitGuid);
    }
  }
  if (uintptr_t pet = localPet())
  {
    // client+0x190 can be empty for a pet loaded from the save, so the unit itself is the
    // authority: its definition GUID is what the copy is built from.
    info.petType = readAscii(gameClient() + engine::kClientPetType);
    info.petName = readName(pet + engine::kUnitName);
    info.petGuid = engine::field<uint64_t>(reinterpret_cast<void*>(pet), engine::kUnitGuid);
    info.petLevel = static_cast<uint16_t>(engine::field<int>(reinterpret_cast<void*>(pet), engine::kUnitLevel));
  }
  return info;
}

bool readLocalPlayerState(net::PlayerState& out)
{
  uintptr_t player = localPlayer();
  if (!player)
    return false;
  Vec3 p = unitPosition(reinterpret_cast<engine::Unit*>(player));
  out.pos[0] = p.x;
  out.pos[1] = p.y;
  out.pos[2] = p.z;
  out.yaw = 0.0f;
  uint32_t node;
  if (g_getDerivedOrientation && diag::readU32(player + engine::kUnitNode, node) && node)
  {
    const ogre::Quaternion& q = g_getDerivedOrientation(reinterpret_cast<const void*>(node));
    out.yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
  }
  out.levelId = levelId(currentLevel());
  out.inGame = inGameplay();
  auto* me = reinterpret_cast<engine::Unit*>(player);
  out.health = engine::field<float>(me, engine::kUnitHealth);
  out.maxHealth = static_cast<uint32_t>(engine::GetMaxHealth(me));
  int ai = engine::field<int>(me, engine::kUnitAIState);
  out.dead = ai == engine::kAIStateDying || ai == engine::kAIStateDead;
  out.hardcore = engine::field<uint8_t>(me, engine::kPlayerHardcore) != 0;
  const float* m = reinterpret_cast<const float*>(player + engine::kUnitTransform);
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      out.rot[r * 3 + c] = m[r * 4 + c];

  if (uintptr_t pet = localPet())
  {
    Vec3 pp = unitPosition(reinterpret_cast<engine::Unit*>(pet));
    out.hasPet = true;
    out.petPos[0] = pp.x;
    out.petPos[1] = pp.y;
    out.petPos[2] = pp.z;
    out.petHealth = engine::field<float>(reinterpret_cast<engine::Unit*>(pet), engine::kUnitHealth);
    out.petMaxHealth = static_cast<uint32_t>(engine::GetMaxHealth(reinterpret_cast<engine::Unit*>(pet)));
  }
  return true;
}

// --- puppet lifecycle ----------------------------------------------------------------------

static void release(Puppet& p, uint8_t slot, const char* what)
{
  if (!p.unit)
    return;
  TLMP_LOG("game: despawning %s for slot %u", what, slot);
  if (&p == &g_avatars[slot].player)
    unregisterClientEntry(g_avatars[slot]);
  // Same sequence SetPlayerClass/SetPetType use to retire the old unit. p.level is
  // non-null only while that level is alive (onLevelDestroyed clears it).
  if (p.level)
    engine::RemoveUnit(p.level, p.unit, false);
  engine::destroyUnit(p.unit);
  p = {};
  if (&p == &g_avatars[slot].player)
  {
    for (void* item : g_avatars[slot].removedItems)
      engine::destroyUnit(item);
    g_avatars[slot].removedItems.clear();
  }
}

// Sets a unit's display name (+0x420) through MSVCP90's own std::wstring::assign, so the
// string is allocated by the CRT the game will later free it with.
static void setUnitName(engine::Unit* unit, const std::string& name)
{
  using AssignFn = void*(__thiscall*)(void* str, const wchar_t* text);
  static AssignFn assign = reinterpret_cast<AssignFn>(GetProcAddress(
    GetModuleHandleW(L"msvcp90.dll"), "?assign@?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAEAAV12@PB_W@Z"));
  if (!assign || !unit || name.empty())
    return;
  std::wstring w = fromUtf8(name);
  assign(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(unit) + engine::kUnitName), w.c_str());
}

// Turns an idle avatar to the remote character's facing (walking avatars face their path).
static void applyFacing(Puppet& p, const float* rot)
{
  float m[16];
  std::memcpy(m, reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(p.unit) + engine::kUnitTransform), sizeof(m));
  float diff = 0.0f;
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
    {
      diff += std::fabs(m[r * 4 + c] - rot[r * 3 + c]);
      m[r * 4 + c] = rot[r * 3 + c];
    }
  if (diff > 0.05f)
    engine::setTransform(p.unit, m, false);
}

// Gives an avatar the remote character's equipment so it looks the same. Items are created
// and slotted the way the game hands out starter gear (0x4D8770): CreateItem on the unit's
// resource manager, then the inventory's own place-in-slot routine.
static void equip(engine::Unit* unit, const std::array<uint64_t, net::kEquipSlots>& gear, uint8_t slot)
{
  void* resources = engine::field<void*>(unit, engine::kUnitResources);
  void* inv = engine::field<void*>(unit, engine::kUnitInventory);
  if (!resources || !inv)
    return;
  // Avatars are fresh level-1 characters; with requirements enforced the game moves any
  // gear they lack the stats for into the backpack. They only need to look right.
  *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(unit) + engine::kEnforceRequirements) = 0;

  for (int s = 0; s < net::kEquipSlots; s++)
  {
    void* current = engine::GetItemInSlot(inv, s);
    uint64_t currentGuid = current ? engine::field<uint64_t>(current, engine::kUnitGuid) : 0;
    if (currentGuid == gear[s])
      continue; // already wearing it (CreatePlayer hands out the class's default gear)
    if (current)
    {
      engine::RemoveItem(inv, current);
      if (engine::GetItemInSlot(inv, s) == current)
      {
        TLMP_LOG("game: slot %u: could not clear equipment slot %d", slot, s);
        continue; // never free an item the inventory still references
      }
      // Not freed here: kept until the avatar itself is destroyed, in case the character
      // still references the item it just unequipped.
      g_avatars[slot].removedItems.push_back(current);
    }
    if (!gear[s])
      continue;

    void* item = engine::CreateUnitByGuid(resources, static_cast<uint32_t>(gear[s]),
                                          static_cast<uint32_t>(gear[s] >> 32), 0, 1, 0);
    if (!item)
    {
      TLMP_LOG("game: slot %u: unknown item GUID %016llx for equipment slot %d", slot, gear[s], s);
      continue;
    }
    // Returns the item on success, null if the slot refused it (nothing was taken over).
    if (!engine::PlaceItemInSlot(inv, item, s, 1))
    {
      TLMP_LOG("game: slot %u: equipment slot %d refused item %016llx", slot, s, gear[s]);
      engine::destroyUnit(item);
    }
  }
}

static void place(Puppet& p, engine::Unit* unit, engine::Level* level, const Vec3& pos)
{
  engine::AddUnit(level, unit, &pos, false);
  p.unit = unit;
  p.level = level;
  p.lastOrder = pos;
  p.lastOrderTime = g_clock;
}

static bool g_creatingStandIn = false;

bool creatingStandIn()
{
  return g_creatingStandIn;
}

// Mirrors CGameClient::SetPlayerClass (0x40CCA0), minus starter items and UI binding.
static void spawnPlayer(uint8_t slot, engine::Level* level)
{
  Avatar& a = g_avatars[slot];
  void* factory = engine::field<void*>(level, engine::kLevelUnitFactory);
  if (!factory)
    return;
  std::wstring cls(a.info.className.begin(), a.info.className.end());
  struct Building
  {
    Building() { g_creatingStandIn = true; }
    ~Building() { g_creatingStandIn = false; }
  } building; // the game links new players to its quest manager somewhere in here
  engine::Unit* unit = engine::CreatePlayer(factory, cls.c_str(), 0);
  if (!unit)
  {
    TLMP_LOG("game: CreatePlayer('%s') failed for slot %u", a.info.className.c_str(), slot);
    return;
  }
  engine::SetAlignment(unit, engine::kAlignmentPlayer);
  setUnitName(unit, a.info.charName);
  equip(unit, a.info.equipment, slot);
  int wanted = 0, matched = 0;
  void* inv = engine::field<void*>(unit, engine::kUnitInventory);
  for (int s = 0; inv && s < net::kEquipSlots; s++)
  {
    void* it = engine::GetItemInSlot(inv, s);
    wanted += a.info.equipment[s] ? 1 : 0;
    matched += (it ? engine::field<uint64_t>(it, engine::kUnitGuid) : 0) == a.info.equipment[s] && a.info.equipment[s];
  }
  Vec3 pos{a.state.pos[0], a.state.pos[1], a.state.pos[2]};
  place(a.player, unit, level, pos);
  registerClientEntry(a, slot);
  TLMP_LOG("game: spawned %s avatar for slot %u @%p at (%.2f, %.2f, %.2f), %d/%d equipment slots match",
           a.info.className.c_str(), slot, unit, pos.x, pos.y, pos.z, matched, wanted);
}

// Mirrors CGameClient::SetPetType (0x40CDE0), but built from the pet's definition GUID.
static void spawnPet(uint8_t slot, engine::Level* level)
{
  Avatar& a = g_avatars[slot];
  void* factory = engine::field<void*>(level, engine::kLevelUnitFactory);
  if (!factory)
    return;
  engine::Unit* unit = engine::createMonsterByGuid(factory, a.info.petGuid, a.info.petLevel);
  if (!unit)
  {
    TLMP_LOG("game: cannot create slot %u's pet (GUID %016llx, '%s')", slot, a.info.petGuid, a.info.petType.c_str());
    a.petFailedGuid = a.info.petGuid; // don't retry every tick
    return;
  }
  engine::SetAlignment(unit, engine::kAlignmentPlayer);
  // Deliberately NOT pet mode (+0x65C = 2): in pet mode the game adopts the copy as the local
  // player's pet/minion and saves it. Its AI is off anyway; it only follows its owner's state.
  setUnitName(unit, a.info.petName);
  Vec3 pos{a.state.petPos[0], a.state.petPos[1], a.state.petPos[2]};
  place(a.pet, unit, level, pos);
  refreshClientEntry(a);
  TLMP_LOG("game: spawned %s pet (GUID %016llx) for slot %u @%p at (%.2f, %.2f, %.2f)", a.info.petType.c_str(),
           a.info.petGuid, slot, unit, pos.x, pos.y, pos.z);
}

// Shows the remote character's health on its stand-in (hover health bar, and what monsters see).
// Max health comes from the base fields, which on a stand-in nothing else sets; the stand-in's
// own stat bonuses are tiny compared to the real character's, so the max shown is the real one.
static void applyVitals(engine::Unit* unit, float health, uint32_t maxHealth)
{
  if (!unit || !maxHealth)
    return;
  auto base = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(unit) + engine::kUnitBaseHealth);
  auto bonus = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(unit) + engine::kUnitBonusHealth);
  if (*base != static_cast<int>(maxHealth) || *bonus != 0)
  {
    *base = static_cast<int>(maxHealth);
    *bonus = 0;
  }
  float max = static_cast<float>(engine::GetMaxHealth(unit));
  float h = health < 1.0f ? 1.0f : health; // never 0 on a stand-in: that is the game's "dead"
  *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(unit) + engine::kUnitHealth) = h > max ? max : h;
}

static void* g_updatingPlayer = nullptr;
static bool g_killingStandIn = false;

void setUpdatingPlayer(void* player)
{
  g_updatingPlayer = player;
}

bool allowPlayerUi()
{
  if (g_killingStandIn)
    return false;
  return !g_updatingPlayer || reinterpret_cast<uintptr_t>(g_updatingPlayer) == localPlayer();
}

bool killingStandIn()
{
  return g_killingStandIn;
}

static bool isDeadUnit(engine::Unit* unit)
{
  int s = engine::field<int>(unit, engine::kUnitAIState);
  return s == engine::kAIStateDying || s == engine::kAIStateDead;
}

// The remote player died: so does their stand-in, with the game's own death (animation, corpse;
// monsters stop going for it). The death screen and tips stay with the player who died.
static void killStandIn(Avatar& a, uint8_t slot)
{
  engine::Unit* unit = a.player.unit;
  if (!unit || isDeadUnit(unit))
    return;
  *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(unit) + engine::kPlayerHardcore) = 0; // never saves
  g_killingStandIn = true;
  engine::kill(unit);
  g_killingStandIn = false;
  TLMP_LOG("game: slot %u's stand-in died (AI state %d)", slot, engine::field<int>(unit, engine::kUnitAIState));
}

// Returns true when the puppet is standing at its target (so facing can be applied).
bool steer(Puppet& p, const Vec3& target)
{
  Vec3 cur = unitPosition(p.unit);
  float dist = distXZ(cur, target);

  bool moving = engine::field<bool>(p.unit, engine::kUnitMoving);
  if (dist > kTeleportDistance)
  {
    engine::setPosition(p.unit, target);
    p.lastOrder = target;
    return false;
  }
  if (g_clock - p.lastOrderTime < kOrderInterval)
    return !moving && dist <= kArriveDistance;

  bool targetMoved = distXZ(target, p.lastOrder) > kReorderDistance;
  if (dist > kArriveDistance && (targetMoved || !moving))
  {
    engine::MoveTo(p.unit, p.level, target.x, target.z);
    p.lastOrder = target;
    p.lastOrderTime = g_clock;
    return false;
  }
  return !moving && dist <= kArriveDistance;
}

static void logStatus(engine::Level* level, uint32_t localLevel)
{
  static float nextReport = 0.0f;
  if (g_clock < nextReport)
    return;
  nextReport = g_clock + 3.0f;

  Vec3 me{};
  if (uintptr_t p = localPlayer())
    me = unitPosition(reinterpret_cast<engine::Unit*>(p));
  TLMP_LOG("game: [status] level %p id %08x me (%.2f, %.2f, %.2f)", level, localLevel, me.x, me.y, me.z);
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
  {
    const Avatar& a = g_avatars[s];
    if (!a.hasState && a.info.className.empty())
      continue;
    TLMP_LOG("game: [status]   slot %u %s+%s (pet GUID %016llx, has pet %d) inGame %d level %08x at (%.2f, %.2f, %.2f) "
             "unit %p pet %p",
             s, a.info.className.c_str(), a.info.petType.c_str(), a.info.petGuid, a.state.hasPet, a.state.inGame,
             a.state.levelId, a.state.pos[0], a.state.pos[1], a.state.pos[2], a.player.unit, a.pet.unit);
  }
}

static void updateAvatars()
{
  engine::Level* level = currentLevel();
  bool playing = level && inGameplay();
  uint32_t localLevel = playing ? levelId(level) : 0;
  logStatus(level, localLevel);

  for (uint8_t slot = 0; slot < net::kMaxPlayers; slot++)
  {
    Avatar& a = g_avatars[slot];
    bool here = playing && a.hasState && a.state.inGame && a.state.levelId == localLevel;
    if (a.hasState && a.state.dead != a.wasDead)
    {
      a.wasDead = a.state.dead;
      if (here)
        showNotice(a.info.charName + (a.state.dead ? " has died." : " is back."), 4.0f);
      TLMP_LOG("game: slot %u %s", slot, a.state.dead ? "died" : "is alive again");
    }
    // A dead player's stand-in dies too and lies there; it is rebuilt when they come back. Nobody
    // dead is spawned (someone arriving later just sees nothing until the player is back).
    bool standInDead = a.player.unit && isDeadUnit(a.player.unit);
    bool wantPlayer = here && !a.info.className.empty() && (!a.state.dead || a.player.unit) &&
                      !(standInDead && !a.state.dead);
    bool wantPet = wantPlayer && a.state.hasPet && a.info.petGuid && a.petFailedGuid != a.info.petGuid;

    if (a.player.unit && (!wantPlayer || a.player.level != level))
      release(a.player, slot, "avatar");
    if (a.pet.unit && (!wantPet || a.pet.level != level))
      release(a.pet, slot, "pet");

    if (wantPlayer && !a.player.unit)
      spawnPlayer(slot, level);
    if (wantPet && !a.pet.unit)
      spawnPet(slot, level);

    if (a.state.dead)
      killStandIn(a, slot);
    else
    {
      if (a.player.unit && steer(a.player, {a.state.pos[0], a.state.pos[1], a.state.pos[2]}))
        applyFacing(a.player, a.state.rot);
      applyVitals(a.player.unit, a.state.health, a.state.maxHealth);
    }
    if (a.pet.unit)
      steer(a.pet, {a.state.petPos[0], a.state.petPos[1], a.state.petPos[2]});
    applyVitals(a.pet.unit, a.state.petHealth, a.state.petMaxHealth);

    // Monsters, spawn areas and warpers see the players registered as game clients. Every machine in
    // a dungeon registers the stand-ins: on the floor's owner monsters go for everyone, and on the
    // others spawn areas still fire (their spawns are refused) as anyone walks by, so whoever runs
    // the floor next has the same areas used up and spawns nothing twice.
    bool runHere = standInsCountHere();
    if (a.clientEntry && (!runHere || !a.player.unit))
      unregisterClientEntry(a);
    else if (!a.clientEntry && runHere && a.player.unit)
      registerClientEntry(a, slot);
  }
}

// --- host: remote players count as players ------------------------------------------
// Trigger volumes, warpers and openables loop over the game clients registered with the
// resource manager and test each client's player (+0x2C). On the host each remote player gets
// a stand-in entry there, so spawn triggers, area events and the like react to them exactly as
// to the local player. The entry is zero-filled apart from the fields those loops use, so any
// unexpected reader fails loudly (crash log) rather than corrupting the real client.

static bool g_isHost = false;

static bool standInsCountHere()
{
  engine::Level* level = currentLevel();
  return level && !floor::isTownLevel(levelId(level));
}

void setHostRole(bool isHost)
{
  g_isHost = isHost;
}

static uintptr_t resourceManager()
{
  uintptr_t client = gameClient();
  return client ? engine::field<uintptr_t>(reinterpret_cast<void*>(client), engine::kClientResources) : 0;
}

static void unregisterClientEntry(Avatar& a)
{
  if (!a.clientEntry)
    return;
  uintptr_t rm = resourceManager();
  if (rm)
  {
    auto** items = engine::field<uint8_t**>(reinterpret_cast<void*>(rm), engine::kResClients);
    auto* count = reinterpret_cast<uint32_t*>(rm + engine::kResClientCount);
    for (uint32_t i = 1; items && i < *count; i++)
    {
      if (items[i] != a.clientEntry)
        continue;
      for (uint32_t j = i; j + 1 < *count; j++)
        items[j] = items[j + 1];
      (*count)--;
      break;
    }
  }
  delete[] a.clientEntry;
  a.clientEntry = nullptr;
}

static void registerClientEntry(Avatar& a, uint8_t slot)
{
  if (!standInsCountHere() || a.clientEntry || !a.player.unit)
    return;
  uintptr_t rm = resourceManager();
  if (!rm)
    return;
  auto** items = engine::field<uint8_t**>(reinterpret_cast<void*>(rm), engine::kResClients);
  auto* count = reinterpret_cast<uint32_t*>(rm + engine::kResClientCount);
  uint32_t capacity = engine::field<uint32_t>(reinterpret_cast<void*>(rm), engine::kResClientCapacity);
  if (!items || *count == 0 || *count >= capacity)
  {
    TLMP_LOG("game: no room to register slot %u as a player (count %u, capacity %u)", slot, *count, capacity);
    return;
  }
  a.clientEntry = new uint8_t[engine::kClientSize]();
  auto set = [&](uint32_t off, const void* v) { std::memcpy(a.clientEntry + off, &v, sizeof(v)); };
  set(engine::kClientPlayer, a.player.unit);
  set(engine::kClientPet, a.pet.unit);
  set(engine::kClientLevel, a.player.level);
  set(engine::kClientResources, reinterpret_cast<void*>(rm));
  items[(*count)++] = a.clientEntry;
  TLMP_LOG("game: registered slot %u's avatar as a player (%u clients)", slot, *count);
}

static void refreshClientEntry(Avatar& a)
{
  if (!a.clientEntry)
    return;
  void* pet = a.pet.unit;
  std::memcpy(a.clientEntry + engine::kClientPet, &pet, sizeof(pet));
}

engine::Unit* avatarOf(uint8_t slot)
{
  return slot < net::kMaxPlayers ? g_avatars[slot].player.unit : nullptr;
}

engine::Unit* avatarPetOf(uint8_t slot)
{
  return slot < net::kMaxPlayers ? g_avatars[slot].pet.unit : nullptr;
}

int avatarSlotOf(const void* unit, bool* isPet)
{
  for (int s = 0; unit && s < net::kMaxPlayers; s++)
  {
    if (g_avatars[s].player.unit == unit || g_avatars[s].pet.unit == unit)
    {
      if (isPet)
        *isPet = g_avatars[s].pet.unit == unit;
      return s;
    }
  }
  return -1;
}

bool isRegisteredClient(const void* client)
{
  for (const Avatar& a : g_avatars)
    if (client && a.clientEntry == client)
      return true;
  return false;
}

bool isAvatarUnit(const void* unit)
{
  for (const Avatar& a : g_avatars)
    if (unit && (a.player.unit == unit || a.pet.unit == unit))
      return true;
  return false;
}

void onLevelDestroyed(void* level)
{
  g_layoutSeeds.erase(level);
  for (Avatar& a : g_avatars)
    if (a.player.level == level)
      unregisterClientEntry(a);

  // The CLevel destructor deletes every unit in its owned list, including our puppets;
  // drop the pointers before that happens so we never touch freed memory.
  for (uint8_t slot = 0; slot < net::kMaxPlayers; slot++)
  {
    for (Puppet* p : {&g_avatars[slot].player, &g_avatars[slot].pet})
    {
      if (p->unit && p->level == level)
      {
        TLMP_LOG("game: level %p destroyed; slot %u unit %p released with it", level, slot, p->unit);
        *p = {};
      }
    }
  }
}

uint32_t onLevelLayoutSeeded(void* level, uint32_t seed)
{
  // seed = level[+0x140] + level[+0x194] + dungeonTerm, where level+0x194 is a copy of the
  // character's world seed (client+0xFB0). If a host seed is active but this level was
  // set up before we adopted it, swap the term here and fix the copies the later derived
  // reseeds (0x4FF85E, 0x40D250) read.
  if (g_hostWorldSeed)
  {
    auto* own = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(level) + engine::kLevelWorldSeed);
    if (*own != g_hostWorldSeed)
    {
      seed = seed - *own + g_hostWorldSeed;
      *own = g_hostWorldSeed;
      applyHostWorldSeed();
    }
  }
  g_layoutSeeds[level] = seed;
  TLMP_LOG("game: level %p layout seed %u%s", level, seed, g_hostWorldSeed ? " (host world)" : "");
  return seed;
}

uint32_t localWorldSeed()
{
  uintptr_t client = gameClient();
  uint32_t seed = 0;
  return client && localPlayer() && diag::readU32(client + engine::kClientWorldSeed, seed) ? seed : 0;
}

void onHostWorldSeed(uint32_t seed)
{
  uintptr_t client = gameClient();
  auto* field = client ? reinterpret_cast<uint32_t*>(client + engine::kClientWorldSeed) : nullptr;

  if (seed == 0)
  {
    if (field && g_hostWorldSeed && *field == g_hostWorldSeed)
    {
      *field = g_ownWorldSeed;
      TLMP_LOG("game: restored own world seed %u", g_ownWorldSeed);
    }
    g_hostWorldSeed = 0;
    return;
  }
  TLMP_LOG("game: host world seed is %u", seed);
  g_hostWorldSeed = seed; // applied by applyHostWorldSeed on the next tick
}

// --- the local player's minions ---------------------------------------------------------------
// A pet copy we create for a remote player gets adopted into the local player's minion list
// (player+0x5C4) -- probably because it is in pet mode -- and SaveCharacter saves every minion.
// So a remote player's pet ended up in the host's save and came back as an extra pet on every
// load. Keep the list to the player's own pet and summons, and get rid of stray pets already
// restored from a polluted save.
static void cleanMinions()
{
  // Only ever removes units we created for remote players. It must never judge which pet is
  // "the player's": right after a floor change client+0x30 doesn't point at the real pet yet,
  // and an earlier version deleted players' own pets that way.
  uintptr_t player = localPlayer();
  if (!player)
    return;
  auto** begin = engine::field<void**>(reinterpret_cast<void*>(player), engine::kPlayerMinions);
  auto*** endField = reinterpret_cast<void***>(player + engine::kPlayerMinions + 4);
  if (!begin || !*endField || *endField < begin)
    return;
  void** out = begin;
  for (void** it = begin; it < *endField; ++it)
  {
    if (*it && isAvatarUnit(*it))
    {
      TLMP_LOG("game: took a remote player's pet copy %p out of the local player's minions", *it);
      continue;
    }
    *out++ = *it;
  }
  *endField = out;
}

void beforeSave(void* client)
{
  // Nothing belonging to remote players may be on the floor or in the minion list when the
  // game saves; avatars come back on the next tick.
  for (uint8_t slot = 0; slot < net::kMaxPlayers; slot++)
  {
    release(g_avatars[slot].pet, slot, "pet (saving)");
    release(g_avatars[slot].player, slot, "avatar (saving)");
  }
  cleanMinions();
  auto* field = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(client) + engine::kClientWorldSeed);
  if (g_hostWorldSeed && *field == g_hostWorldSeed)
    *field = g_ownWorldSeed;
}

void afterSave(void* client)
{
  auto* field = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(client) + engine::kClientWorldSeed);
  if (g_hostWorldSeed && *field == g_ownWorldSeed)
    *field = g_hostWorldSeed;
}

// Keeps client+0xFB0 equal to the host's value. Re-checked every frame because loading a
// character (0x415A50) reads the field back from the save.
static void applyHostWorldSeed()
{
  if (!g_hostWorldSeed || !localPlayer())
    return;
  auto* field = reinterpret_cast<uint32_t*>(gameClient() + engine::kClientWorldSeed);
  if (*field == g_hostWorldSeed)
    return;
  g_ownWorldSeed = *field;
  *field = g_hostWorldSeed;
  TLMP_LOG("game: world seed %u -> host's %u (new floors will match the host)", g_ownWorldSeed, g_hostWorldSeed);
}

// --- chat ------------------------------------------------------------------------------
// Text travels as UTF-8. What can actually be drawn depends on the game's fonts: the HUD fonts
// (Torchlight Regular, BRLNSDB) cover Latin-1, the console font (Consolas) adds Latin
// Extended-A, Greek and Cyrillic; no shipped font has CJK glyphs.

static void (*g_chatSend)(const std::string&) = nullptr;
static std::string g_localName = "me";
static bool g_sessionActive = false;

void setChatSender(void (*send)(const std::string& text))
{
  g_chatSend = send;
}

std::string displayName(uint8_t slot, const std::string& fallback)
{
  if (slot < net::kMaxPlayers && !g_avatars[slot].info.charName.empty())
    return g_avatars[slot].info.charName;
  return fallback;
}

static void* gameUI()
{
  uintptr_t client = gameClient();
  return client ? engine::field<void*>(reinterpret_cast<void*>(client), engine::kClientGameUI) : nullptr;
}

static void consolePrint(const std::wstring& line)
{
  if (void* ui = gameUI())
    if (void* console = engine::field<void*>(ui, engine::kGameUIConsole))
      engine::ConsolePrint(console, vs::make(line.c_str()));
}

void showNotice(const std::string& utf8, float seconds)
{
  autotest::onNotice(utf8);
  if (void* ui = gameUI())
    engine::ShowSmallMessage(ui, vs::make(fromUtf8(utf8).c_str()), seconds);
}

void showChat(const std::string& who, const std::string& text)
{
  TLMP_LOG("chat: <%s> %s", who.c_str(), text.c_str());
  void* ui = gameUI();
  if (!ui)
    return;
  std::wstring line = fromUtf8(who) + L": " + fromUtf8(text);
  engine::ShowSmallMessage(ui, vs::make(line.c_str()), 8.0f);
  consolePrint(line);
}

// Console commands that only affect the local view/logs. Everything else in the developer
// console changes game state (GOD, KILLALL, MONEY, LEVELUP, ASCEND, ...) and would break the
// shared game, so it is refused while a multiplayer session is on.
static bool isAllowedInSession(const std::wstring& line)
{
  // The dispatcher skips leading characters below 'A' and compares case-insensitively.
  size_t i = 0;
  while (i < line.size() && line[i] < L'A')
    i++;
  std::wstring cmd;
  for (; i < line.size() && line[i] != L' '; i++)
    cmd.push_back(static_cast<wchar_t>(towupper(line[i])));
  static const wchar_t* const allowed[] = {L"HELP", L"CLS", L"FPS", L"ROOM", L"SHOWUI", L"HIDEUI", L"ACHIEVEMENTS"};
  if (cmd.empty() && line.find(L'?') != std::wstring::npos)
    return true; // "?" is an alias for HELP
  for (const wchar_t* a : allowed)
    if (cmd == a)
      return true;
  return cmd.empty();
}

bool onConsoleLine(const wchar_t* line)
{
  std::wstring w(line);
  std::wstring msg;
  if (!w.empty() && w[0] == L'/')
    msg = w.substr(1);
  else if (w.size() > 4 && _wcsnicmp(w.c_str(), L"say ", 4) == 0)
    msg = w.substr(4);
  else if (g_sessionActive && !isAllowedInSession(w) && !autotest::active()) // tests may use them
  {
    TLMP_LOG("game: blocked console command during session");
    consolePrint(L"Cheat commands are disabled during multiplayer.");
    return true;
  }
  else
    return false;

  if (msg.empty() || !g_chatSend)
    return true;
  std::string text = toUtf8(msg.c_str());
  g_chatSend(text);
  showChat(g_localName, text); // local echo
  return true;
}

// --- console settings ------------------------------------------------------------------
// While a session is on, CONSOLE and CONSOLE NOPAUSE are switched on in memory only. The
// player's own values are written whenever the game saves settings, and put back for good
// when the session ends, so settings.txt never changes because of us.

static uint32_t g_consoleOriginal[2];
static bool g_consoleOverridden = false;
// CSettings captured when the session starts: at shutdown the game saves settings after the
// master resource manager (our usual way to reach CSettings) is already gone.
static uintptr_t g_settings = 0;

static uint32_t* settingSlot(uintptr_t settings, uintptr_t idGlobal)
{
  uint32_t begin = 0, end = 0, id = 0;
  if (!diag::readU32(settings + engine::kSettingsValues, begin) ||
      !diag::readU32(settings + engine::kSettingsValues + 4, end) || !begin || end <= begin ||
      !diag::readU32(idGlobal, id) || id >= (end - begin) / 4)
    return nullptr;
  return reinterpret_cast<uint32_t*>(begin + id * 4);
}

static uintptr_t settingsObject()
{
  uint32_t mrm = 0, settings = 0;
  if (!diag::readU32(engine::kMasterResourceManager, mrm) || !diag::readU32(mrm + engine::kMrmSettings, settings))
    return 0;
  return settings;
}

static const uintptr_t kConsoleSettingIds[2] = {engine::kSettingConsoleId, engine::kSettingConsoleNoPauseId};

void enableConsoleForSession()
{
  g_sessionActive = true;
  uintptr_t settings = settingsObject();
  uint32_t* slots[2] = {settingSlot(settings, kConsoleSettingIds[0]), settingSlot(settings, kConsoleSettingIds[1])};
  if (!settings || !slots[0] || !slots[1])
    return;
  if (!g_consoleOverridden)
  {
    g_settings = settings;
    g_consoleOriginal[0] = *slots[0];
    g_consoleOriginal[1] = *slots[1];
    g_consoleOverridden = true;
  }
  *slots[0] = 1;
  *slots[1] = 1;
}

static void putBackConsoleSettings(uintptr_t settings)
{
  for (int i = 0; i < 2; i++)
    if (uint32_t* slot = settingSlot(settings, kConsoleSettingIds[i]))
      *slot = g_consoleOriginal[i];
}

void endSession()
{
  g_sessionActive = false;
  if (g_consoleOverridden)
    putBackConsoleSettings(g_settings);
  g_consoleOverridden = false;
}

void beforeSettingsWrite(void* settings)
{
  if (g_consoleOverridden && reinterpret_cast<uintptr_t>(settings) == g_settings)
  {
    putBackConsoleSettings(reinterpret_cast<uintptr_t>(settings));
    TLMP_LOG("game: settings save: writing the player's own console settings (%u, %u)", g_consoleOriginal[0],
             g_consoleOriginal[1]);
  }
}

void afterSettingsWrite(void* settings)
{
  if (g_consoleOverridden && reinterpret_cast<uintptr_t>(settings) == g_settings)
    enableConsoleForSession();
}

// --- frame tick ------------------------------------------------------------------------

void tick(float dt)
{
  g_clock += dt;

  if (!g_getDerivedOrientation)
  {
    if (HMODULE ogreMain = GetModuleHandleW(L"OgreMain.dll"))
      g_getDerivedOrientation = reinterpret_cast<ogre::GetDerivedOrientationFn>(
        GetProcAddress(ogreMain, "?_getDerivedOrientation@Node@Ogre@@UBEABVQuaternion@2@XZ"));
  }

  // Log each level's fingerprint once it has settled (a few seconds after it appears).
  static engine::Level* fpLevel = nullptr;
  static float fpAt = 0.0f;
  if (engine::Level* lvl = currentLevel(); lvl != fpLevel)
  {
    fpLevel = lvl;
    fpAt = g_clock + 4.0f;
  }
  else if (fpAt > 0.0f && g_clock >= fpAt && lvl)
  {
    fpAt = 0.0f;
    uint32_t count = 0;
    uint32_t fp = sceneFingerprint(&count);
    TLMP_LOG("game: level %p id %08x scene fingerprint %08x (%u nodes)", lvl, levelId(lvl), fp, count);
  }

  uintptr_t player = localPlayer();
  if (player != g_lastLocalPlayer)
  {
    g_lastLocalPlayer = player;
    if (player)
    {
      net::PlayerInfo info = localPlayerInfo();
      TLMP_LOG("game: local player %08x %s+%s (pet GUID %016llx) level %08x", static_cast<unsigned>(player),
               info.className.c_str(), info.petType.c_str(), info.petGuid, levelId(currentLevel()));
    }
  }

  if (net::PlayerInfo info = localPlayerInfo(); !info.charName.empty())
    g_localName = info.charName;
  applyHostWorldSeed();
  updateAvatars();

  static float nextMinionCheck = 0.0f;
  if (g_clock >= nextMinionCheck)
  {
    nextMinionCheck = g_clock + 0.5f;
    cleanMinions();
  }
}

void repairPet(const std::wstring& type, const std::wstring& name)
{
  static bool done = false;
  uintptr_t client = gameClient();
  if (done || !client || !localPlayer() || !currentLevel() || !party::enteredFromContinue())
    return; // only in real gameplay, never on the title screen
  done = true;
  uint32_t pet = 0;
  diag::readU32(client + engine::kClientPet, pet);
  if (pet && diag::rttiName(pet))
  {
    TLMP_LOG("game: repairpet: character already has a pet; nothing to do");
    return;
  }
  // SetPetType only acts when the type differs from the stored one (client+0x190), which can
  // still name the lost pet; clear it first.
  wchar_t stored[64] = L"";
  diag::readWStringAny(client + engine::kClientPetType, stored, 64);
  TLMP_LOG("game: repairpet: stored pet type was '%ls'", stored);
  {
    using AssignFn = void*(__thiscall*)(void* str, const wchar_t* text);
    static AssignFn assign = reinterpret_cast<AssignFn>(GetProcAddress(GetModuleHandleW(L"msvcp90.dll"),
      "?assign@?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@QAEAAV12@PB_W@Z"));
    if (assign)
      assign(reinterpret_cast<void*>(client + engine::kClientPetType), L"");
  }
  engine::SetPetType(reinterpret_cast<void*>(client), vs::make(type.c_str()));
  diag::readU32(client + engine::kClientPet, pet);
  if (!pet)
  {
    TLMP_LOG("game: repairpet: SetPetType made no pet for type '%ls'", type.c_str());
    return;
  }
  auto* petUnit = reinterpret_cast<engine::Unit*>(pet);
  auto* player = reinterpret_cast<engine::Unit*>(localPlayer());
  setUnitName(petUnit, toUtf8(name.c_str()));
  // The rest of what console FIXPET does: without AddMinion the pet is saved as a floor unit.
  *reinterpret_cast<uint8_t*>(pet + engine::kUnitInvincible) = 1;
  engine::AddMinion(player, petUnit);
  TLMP_LOG("game: repairpet: created %ls '%ls' (%08x), minions now %u", type.c_str(), name.c_str(), pet,
           static_cast<unsigned>((engine::field<uintptr_t>(player, engine::kPlayerMinions + 4) -
                                  engine::field<uintptr_t>(player, engine::kPlayerMinions)) / 4));

  // An earlier repair attempt left a copy of this pet in the floor's saved units (never added
  // as a minion). Only exact matches of the pet just created: same UNIT_GUID and the name given
  // on the command line, not an avatar, not in our minion list.
  std::string wantName = toUtf8(name.c_str());
  uint64_t guid = engine::field<uint64_t>(petUnit, engine::kUnitGuid);
  auto* level = currentLevel();
  std::vector<engine::Unit*> strays;
  uint32_t list = 0, node = 0;
  if (diag::readU32(reinterpret_cast<uintptr_t>(level) + engine::kLevelUnits, list) && list &&
      diag::readU32(list, node))
  {
    for (int guard = 0; node && guard < 20000; guard++)
    {
      uint32_t unit = 0, next = 0;
      diag::readU32(node, unit);
      diag::readU32(node + 4, next);
      node = next;
      if (!unit || unit == pet || isAvatarUnit(reinterpret_cast<void*>(unit)))
        continue;
      auto* u = reinterpret_cast<engine::Unit*>(unit);
      const char* cls = diag::rttiName(unit);
      if (!cls || !strstr(cls, "CMonster") || engine::field<uint64_t>(u, engine::kUnitGuid) != guid ||
          readName(unit + engine::kUnitName) != wantName)
        continue;
      auto** it = engine::field<engine::Unit**>(player, engine::kPlayerMinions);
      auto** end = engine::field<engine::Unit**>(player, engine::kPlayerMinions + 4);
      if (std::find(it, end, u) == end)
        strays.push_back(u);
    }
  }
  for (auto* u : strays)
  {
    TLMP_LOG("game: repairpet: removing the stray floor copy %08x of '%ls'", reinterpret_cast<uint32_t>(u),
             name.c_str());
    engine::RemoveUnit(level, u, false);
    engine::destroyUnit(u);
  }
  // No save here: the game's own next save (quit / floor change) writes it together with the
  // rest of the world. Saving from the title screen would drop town, floors and stash.
}

uint32_t currentLevelId()
{
  return levelId(currentLevel());
}

void onRemoteJoined(uint8_t slot, const std::string& name)
{
  TLMP_LOG("game: remote player %u '%s' joined", slot, name.c_str());
  g_avatars[slot] = {};
}

void onRemoteLeft(uint8_t slot)
{
  TLMP_LOG("game: remote player %u left", slot);
  release(g_avatars[slot].pet, slot, "pet");
  release(g_avatars[slot].player, slot, "avatar");
  g_avatars[slot] = {};
}

void onRemoteState(uint8_t slot, const net::PlayerState& st)
{
  Avatar& a = g_avatars[slot];
  if (a.hasState && (st.tick - a.state.tick) > 0x80000000u)
    return; // older than what we have (unreliable channel can reorder)
  a.state = st;
  a.hasState = true;
}

void onRemoteInfo(uint8_t slot, const net::PlayerInfo& info)
{
  Avatar& a = g_avatars[slot];
  // Changed class or pet: drop the stale unit; updateAvatars respawns it next tick.
  if (a.info.className != info.className || a.info.equipment != info.equipment)
    release(a.player, slot, "avatar"); // rebuilt with the new class/gear next tick
  if (a.info.petGuid != info.petGuid)
    release(a.pet, slot, "pet");
  if (a.player.unit && a.info.charName != info.charName)
    setUnitName(a.player.unit, info.charName);
  if (a.pet.unit && a.info.petName != info.petName)
    setUnitName(a.pet.unit, info.petName);
  a.info = info;
}

} // namespace tlmp::game
