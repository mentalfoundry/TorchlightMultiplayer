#include "game/minimap.h"
#include "game/diag.h"
#include "log.h"
#include "net/protocol.h"

#include <windows.h>

#include <cstring>

namespace tlmp::game::minimap {

namespace {

constexpr uint32_t kLevelAutomap = 0x160;
constexpr int kPlayerIcon = 0x11; // the purple orb (media/sharedtextures/automap.dds)
// CAutomap::AddIcon(icon, const Vector3* pos, const Vector3* facing, bool, bool) (0x4EBB40, ret
// 0x14): a billboard in the automap's dynamic set. With both flags false it is not one of the icons
// the automap rebuilds (0x4EB360 removes only those), so it lasts as long as the automap.
using AddIconFn = void*(__thiscall*)(void* automap, int icon, const Vec3* pos, const Vec3* facing, bool, bool);
const auto AddIcon = reinterpret_cast<AddIconFn>(0x004EBB40);
// Where the automap puts the pet's icon to hide it (0x4EA410): x, y, z floats.
constexpr uintptr_t kHideX = 0x00A921A4, kHideY = 0x00AA98E0;

struct ColourValue
{
  float r, g, b, a;
};
using SetPositionFn = void(__thiscall*)(void* billboard, const Vec3* pos);
using SetColourFn = void(__thiscall*)(void* billboard, const ColourValue* colour);
using GetPositionFn = const Vec3*(__thiscall*)(const void* billboard);
SetPositionFn g_setPosition = nullptr;
SetColourFn g_setColour = nullptr;
GetPositionFn g_getPosition = nullptr;

void* g_level = nullptr; // the level the markers below belong to (its automap made them)
void* g_marker[net::kMaxPlayers] = {};
bool g_shown[net::kMaxPlayers] = {};

bool resolve()
{
  static bool tried = false, ok = false;
  if (tried)
    return ok;
  tried = true;
  HMODULE ogre = GetModuleHandleW(L"OgreMain.dll");
  if (!ogre)
    return false;
  g_setPosition = reinterpret_cast<SetPositionFn>(GetProcAddress(ogre, "?setPosition@Billboard@Ogre@@QAEXABVVector3@2@@Z"));
  g_setColour = reinterpret_cast<SetColourFn>(GetProcAddress(ogre, "?setColour@Billboard@Ogre@@QAEXABVColourValue@2@@Z"));
  g_getPosition = reinterpret_cast<GetPositionFn>(GetProcAddress(ogre, "?getPosition@Billboard@Ogre@@QBEABVVector3@2@XZ"));
  ok = g_setPosition && g_setColour && g_getPosition;
  if (!ok)
    TLMP_LOG("minimap: Ogre billboard functions not found; no markers for other players");
  return ok;
}

Vec3 hidden()
{
  float x = -10000.0f, y = -10000.0f;
  uint32_t bits = 0;
  if (diag::readU32(kHideX, bits))
    std::memcpy(&x, &bits, sizeof(x));
  if (diag::readU32(kHideY, bits))
    std::memcpy(&y, &bits, sizeof(y));
  return Vec3{x, y, x};
}

void hide(uint8_t slot)
{
  if (!g_marker[slot] || !g_shown[slot])
    return;
  Vec3 away = hidden();
  g_setPosition(g_marker[slot], &away);
  g_shown[slot] = false;
}

} // namespace

void tick(bool show)
{
  if (!resolve())
    return;
  engine::Level* level = currentLevel();
  if (level != g_level)
  {
    // Another level, another automap: the old markers went with the old one.
    g_level = level;
    for (uint8_t s = 0; s < net::kMaxPlayers; s++)
    {
      g_marker[s] = nullptr;
      g_shown[s] = false;
    }
  }
  void* automap = level ? engine::field<void*>(level, kLevelAutomap) : nullptr;
  for (uint8_t slot = 0; slot < net::kMaxPlayers; slot++)
  {
    engine::Unit* standIn = show && automap ? avatarOf(slot) : nullptr;
    if (!standIn)
    {
      hide(slot);
      continue;
    }
    Vec3 pos = unitPosition(standIn);
    if (!g_marker[slot])
    {
      Vec3 facing{1.0f, 0.0f, 0.0f};
      g_marker[slot] = AddIcon(automap, kPlayerIcon, &pos, &facing, false, false);
      if (!g_marker[slot])
        continue;
      ColourValue blue{0.35f, 0.8f, 1.0f, 1.0f}; // times the purple orb: a blue one
      g_setColour(g_marker[slot], &blue);
      TLMP_LOG("minimap: marker for slot %u on level %p", slot, level);
    }
    g_setPosition(g_marker[slot], &pos);
    g_shown[slot] = true;
  }
}

void onLevelDestroyed(void* level)
{
  if (level != g_level)
    return;
  g_level = nullptr;
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
  {
    g_marker[s] = nullptr;
    g_shown[s] = false;
  }
}

bool markerAt(uint8_t slot, Vec3* at)
{
  if (slot >= net::kMaxPlayers || !g_marker[slot] || !g_shown[slot] || !g_getPosition)
    return false;
  *at = *g_getPosition(g_marker[slot]);
  return true;
}

} // namespace tlmp::game::minimap
