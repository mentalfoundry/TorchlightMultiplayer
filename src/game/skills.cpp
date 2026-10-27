#include "game/skills.h"
#include "game/diag.h"
#include "game/floor.h"
#include "game/internal.h"
#include "game/minions.h"
#include "game/monsters.h"
#include "game/vsstring.h"
#include "log.h"
#include "net/session.h"

#include <cmath>
#include <string>

namespace tlmp::game::skills {

using net::Msg;

static net::Session* g_session = nullptr;
static engine::Unit* g_replaying = nullptr; // stand-in casting a received skill right now
static unsigned g_sent = 0, g_played = 0, g_failed = 0;

constexpr int kAIStateQueued = 13;  // CCharacter AI state: a skill waits to start (seen 13 -> 14 on a cast)
constexpr int kAIStateSkill = 14;   // ... and while it plays
constexpr uint32_t kSkillLevel = 0xA4;
constexpr uint32_t kSkillLevelData = 0x5C; // -> +0xCC: the skill's name (wstring)
constexpr uint32_t kSkillName = 0xCC;
constexpr uint32_t kCharMana = 0x3B8;      // float; UseSkill compares it with the skill's cost
constexpr uint32_t kMinionOwner = 0x5B4;

// 0x4805A0: CCharacter::LearnSkill(const std::wstring* name, int) -> CSkill-ish (0 = unknown), ret 8.
using LearnSkillFn = int(__thiscall*)(engine::Unit* unit, const vs::WString* name, int unused);
static const auto LearnSkill = reinterpret_cast<LearnSkillFn>(0x004805A0);
// 0x5D5E80: CSkill::SetLevel(int level), ret 4.
using SetSkillLevelFn = void(__thiscall*)(void* skill, int level);
static const auto SetSkillLevel = reinterpret_cast<SetSkillLevelFn>(0x005D5E80);

static bool connected()
{
  return g_session && g_session->isConnected();
}

void attach(net::Session* session)
{
  g_session = session;
}

bool learnForTest(engine::Unit* unit, const std::string& name)
{
  vs::WString w = vs::make(fromUtf8(name).c_str());
  LearnSkill(unit, &w, 0);
  // CSkillManager::FindSkill(const std::wstring* name) (0x5DD660).
  void* manager = engine::field<void*>(unit, engine::kCharSkillManager);
  using FindSkillFn = void*(__thiscall*)(void* manager, const vs::WString* name);
  void* skill = manager ? reinterpret_cast<FindSkillFn>(0x005DD660)(manager, &w) : nullptr;
  vs::destroy(&w);
  if (!skill)
    return false;
  if (engine::field<int>(skill, kSkillLevel) < 1)
    SetSkillLevel(skill, 1); // learned skills start uninvested (level 0: they do nothing)
  *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(unit) + engine::kCharRightSkill) =
    engine::field<uint64_t>(skill, engine::kSkillGuid);
  return true;
}

unsigned castsSent()
{
  return g_sent;
}

unsigned castsPlayed()
{
  return g_played;
}

bool isShowOnly(const void* unit)
{
  if (!unit)
    return false;
  if (isAvatarUnit(unit))
    return true;
  const void* owner = engine::field<void*>(unit, kMinionOwner);
  if (owner && isAvatarUnit(owner))
    return true;
  uint8_t slot = 0, index = 0;
  return minions::copyOf(unit, &slot, &index);
}

static std::string skillName(void* skill)
{
  auto* data = engine::field<uint8_t*>(skill, kSkillLevelData);
  wchar_t name[96] = {};
  if (!data || !diag::readWString(reinterpret_cast<uintptr_t>(data) + kSkillName, name, 96))
    return {};
  return toUtf8(name);
}

// Our id for the unit a skill was aimed at, as every machine on the floor knows it (0 = none).
static uint32_t targetId(engine::Unit* target)
{
  if (!target)
    return 0;
  return floor::ownsCurrentLevel() ? monsters::trackedId(target) : monsters::puppetId(target);
}

static engine::Unit* targetById(uint32_t id)
{
  if (!id)
    return nullptr;
  return floor::ownsCurrentLevel() ? monsters::trackedById(id) : monsters::puppetById(id);
}

static void sendCast(engine::Unit* me, void* skill)
{
  uint64_t guid = engine::field<uint64_t>(skill, engine::kSkillGuid);
  int level = engine::field<int>(skill, kSkillLevel);
  std::string name = skillName(skill);
  Vec3 at = engine::field<Vec3>(me, engine::kCharSkillTarget);
  auto* target = engine::field<engine::Unit*>(me, engine::kCharTargetUnit);
  net::Writer w(Msg::SkillCast);
  w.u32(levelId(currentLevel()));
  w.u32(static_cast<uint32_t>(guid));
  w.u32(static_cast<uint32_t>(guid >> 32));
  w.u32(static_cast<uint32_t>(level));
  w.str(name);
  w.f32(at.x);
  w.f32(at.y);
  w.f32(at.z);
  w.u32(targetId(target));
  floor::sendToFloor(w, true);
  if (++g_sent <= 20 || g_sent % 100 == 0)
    TLMP_LOG("skills: I cast '%s' (%016llx, level %d) at (%.1f, %.1f), target %p; %u sent", name.c_str(), guid, level,
             at.x, at.z, target, g_sent);
}

uint32_t useSkill(engine::UseSkillFn original, engine::Unit* unit, uint32_t guidLo, uint32_t guidHi)
{
  if (unit == g_replaying || !connected() || reinterpret_cast<uintptr_t>(unit) != localPlayer())
    return original(unit, guidLo, guidHi);
  int before = engine::field<int>(unit, engine::kUnitAIState);
  uint32_t result = original(unit, guidLo, guidHi);
  // A request that starts the skill moves the character into the skill state; others are refused
  // (0), queued until in range (the game calls again with the current skill), or busy.
  if (result && before != kAIStateSkill && engine::field<int>(unit, engine::kUnitAIState) == kAIStateSkill)
  {
    if (void* skill = engine::field<void*>(unit, engine::kCharCurrentSkill))
      sendCast(unit, skill);
  }
  return result;
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (type != Msg::SkillCast || !connected())
    return;
  uint32_t lid = r.u32();
  uint32_t lo = r.u32(), hi = r.u32();
  int level = static_cast<int>(r.u32());
  std::string name = r.str();
  Vec3 at{r.f32(), r.f32(), r.f32()};
  uint32_t target = r.u32();
  engine::Unit* standIn = avatarOf(fromSlot);
  int ai = standIn ? engine::field<int>(standIn, engine::kUnitAIState) : -1;
  if (!r.ok() || lid != levelId(currentLevel()) || !standIn || ai == engine::kAIStateDying ||
      ai == engine::kAIStateDead)
  {
    TLMP_LOG("skills: slot %u's cast '%s' not shown (%s, state %d)", fromSlot, name.c_str(),
             !r.ok() ? "bad message" : lid != levelId(currentLevel()) ? "another floor" : !standIn ? "no stand-in" : "dead",
             ai);
    return;
  }

  // The stand-in may not know the skill (a fresh character): learn it by name, at the caster's level.
  void* manager = engine::field<void*>(standIn, engine::kCharSkillManager);
  void* skill = manager ? engine::FindSkillByGuid(manager, lo, hi) : nullptr;
  if (!skill && !name.empty())
  {
    vs::WString w = vs::make(fromUtf8(name).c_str());
    LearnSkill(standIn, &w, 0);
    vs::destroy(&w);
    manager = engine::field<void*>(standIn, engine::kCharSkillManager);
    skill = manager ? engine::FindSkillByGuid(manager, lo, hi) : nullptr;
    TLMP_LOG("skills: slot %u's stand-in learned '%s': %s", fromSlot, name.c_str(), skill ? "ok" : "unknown skill");
  }
  if (!skill)
  {
    g_failed++;
    return;
  }
  if (level > 0 && engine::field<int>(skill, kSkillLevel) != level)
    SetSkillLevel(skill, level);

  // Aim as the caster did, give it the mana the cast needs, and cast.
  *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(standIn) + kCharMana) = 1.0e6f;
  engine::AimAt(standIn, at.x, at.z);
  engine::SetTargetUnit(standIn, targetById(target));
  g_replaying = standIn;
  uint32_t result = reinterpret_cast<engine::UseSkillFn>(engine::kUseSkill)(standIn, lo, hi);
  g_replaying = nullptr;
  // Accepted, or queued: the game takes the next steps itself (state 13, then the cast, 14).
  int state = engine::field<int>(standIn, engine::kUnitAIState);
  bool ok = result || state == kAIStateQueued || state == kAIStateSkill;
  if (ok)
    g_played++;
  else
    g_failed++;
  if (g_played + g_failed <= 20 || (g_played + g_failed) % 100 == 0)
    TLMP_LOG("skills: slot %u's stand-in casts '%s' (level %d) at (%.1f, %.1f): %s (state %d); %u shown, %u refused",
             fromSlot, name.c_str(), level, at.x, at.z, ok ? "ok" : "refused", state, g_played, g_failed);
}

} // namespace tlmp::game::skills
