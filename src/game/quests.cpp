#include "game/quests.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/floor.h"
#include "game/internal.h"
#include "game/monsters.h"
#include "log.h"
#include "net/session.h"

#include <cstring>
#include <vector>

namespace tlmp::game::quests {

using net::Msg;

static net::Session* g_session = nullptr;

struct Pending
{
  uint32_t levelId;
  Vec3 pos;
  std::vector<uint8_t> state;
};
static std::vector<Pending> g_pending; // waiting for the floor's owner to be known

static bool connected()
{
  return g_session && g_session->isConnected();
}

static uint64_t questTag(const void* unit, uint32_t* index)
{
  *index = engine::field<uint32_t>(unit, engine::kUnitQuestIndex);
  return engine::field<uint64_t>(unit, engine::kUnitQuestGuid);
}

static bool sendToOwner(const Pending& p)
{
  int owner = floor::currentOwner();
  if (owner < 0 || p.levelId != levelId(currentLevel()))
    return false;
  net::Writer w(Msg::QuestSpawn);
  w.u32(p.levelId);
  w.f32(p.pos.x);
  w.f32(p.pos.y);
  w.f32(p.pos.z);
  w.bytes(p.state);
  floor::sendTo(static_cast<uint8_t>(owner), w, true);
  return true;
}

void attach(net::Session* session)
{
  g_session = session;
}

void* spawn(SpawnFn original, void* questUnit, void* resources, const void* position)
{
  void* level = resources ? engine::field<void*>(resources, 0xC) : nullptr;
  if (!connected() || !level || floor::ownsLevel(level))
  {
    void* unit = original(questUnit, resources, position);
    if (unit && connected())
    {
      uint32_t index = 0;
      uint64_t guid = questTag(unit, &index);
      TLMP_LOG("quests: my game placed quest unit %p (%016llx, quest %016llx #%u) on the floor I run", unit,
               engine::field<uint64_t>(unit, engine::kUnitGuid), guid, index);
    }
    return unit;
  }

  // A floor someone else runs: let the game build the monster as usual (tags and all), then hand it
  // to the owner. Quest items are ours alone and stay.
  monsters::allowLocalSpawns(true);
  void* unit = original(questUnit, resources, position);
  monsters::allowLocalSpawns(false);
  if (!unit || !monsters::isMonster(unit))
    return unit;
  uint32_t index = 0;
  uint64_t guid = questTag(unit, &index);
  Pending p{levelId(static_cast<engine::Level*>(level)), unitPosition(unit), {}};
  if (!monsters::captureState(static_cast<engine::Unit*>(unit), p.state))
  {
    TLMP_LOG("quests: could not save quest unit %p; it stays here only", unit);
    return unit;
  }
  monsters::retireUnit(static_cast<engine::Level*>(level), static_cast<engine::Unit*>(unit));
  TLMP_LOG("quests: my quest %016llx #%u wants %016llx here; asking the floor's owner to create it", guid, index,
           engine::field<uint64_t>(unit, engine::kUnitGuid));
  if (!sendToOwner(p))
    g_pending.push_back(std::move(p));
  return unit; // the quest keeps its reference; the unit is just no longer in the level
}

void tick()
{
  if (!connected() || g_pending.empty())
    return;
  uint32_t here = levelId(currentLevel());
  for (auto it = g_pending.begin(); it != g_pending.end();)
  {
    if (it->levelId != here)
      it = g_pending.erase(it); // we left that floor before anyone could take it
    else if (sendToOwner(*it))
      it = g_pending.erase(it);
    else
      ++it;
  }
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (type != Msg::QuestSpawn || !connected())
    return;
  uint32_t lid = r.u32();
  Vec3 pos{r.f32(), r.f32(), r.f32()};
  std::vector<uint8_t> state = r.bytes();
  if (!r.ok() || lid != levelId(currentLevel()) || !floor::ownsCurrentLevel())
  {
    TLMP_LOG("quests: slot %u's quest unit for level %08x arrived, but I don't run that floor", fromSlot, lid);
    return;
  }
  engine::Unit* unit = monsters::spawnFromState(state, pos);
  if (!unit)
  {
    TLMP_LOG("quests: could not create slot %u's quest unit", fromSlot);
    return;
  }
  uint32_t index = 0;
  uint64_t guid = questTag(unit, &index);
  if (monsters::otherLivingWithQuestTag(unit, guid, index))
  {
    // Someone's quest already put this one here (same quest): one is enough, its kill counts for all.
    monsters::retireUnit(currentLevel(), unit);
    TLMP_LOG("quests: slot %u's quest unit (quest %016llx #%u) is here already", fromSlot, guid, index);
    return;
  }
  TLMP_LOG("quests: created slot %u's quest unit %p (%016llx, quest %016llx #%u) at (%.1f, %.1f)", fromSlot, unit,
           engine::field<uint64_t>(unit, engine::kUnitGuid), guid, index, pos.x, pos.z);
}

} // namespace tlmp::game::quests
