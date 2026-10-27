#pragma once

#include "game/internal.h"

#include <cstdint>

// The other players on the minimap / automap. The automap (CLevel +0x160) draws its icons as
// billboards: the local player's purple orb (+0x40, icon 0x11) and pet (+0x44, icon 0x10) are made
// once by the automap and moved every frame. Each other player whose stand-in is on this floor gets
// the same orb, tinted blue, made with the automap's own AddIcon (0x4EBB40) and moved to the
// stand-in every frame; it is hidden while they are elsewhere.
namespace tlmp::game::minimap {

void tick(bool show); // every frame; `show` false hides every marker (no session)
void onLevelDestroyed(void* level);

// Tests: where slot's marker is (false if it has none or it is hidden).
bool markerAt(uint8_t slot, Vec3* at);

} // namespace tlmp::game::minimap
