#pragma once

namespace tlmp::hooks {

bool install();
void uninstall();

// Treasure rolls the game made for kills (from Kill, engine.h), whatever they dropped -- often nothing.
unsigned killTreasureRolls();

} // namespace tlmp::hooks
