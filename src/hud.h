#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The party frames: one small panel per other player under the pet's portrait (top left) -- name,
// level and a health bar like the pet's, greyed out while they are dead, and a note when they are
// somewhere else (town, another floor, loading).
namespace tlmp::hud {

struct Member
{
  enum class Where
  {
    Here,      // on this floor
    Town,      // in town (and we aren't)
    Elsewhere, // another floor
    Loading,   // on the title screen or loading
  };
  uint8_t slot = 0;
  std::string name;
  int level = 0;
  float health = 0.0f;
  uint32_t maxHealth = 0;
  bool dead = false;
  Where where = Where::Here;
};

// Every frame, main thread: `show` false hides the frames (title screen, no session).
void tick(bool show, const std::vector<Member>& members);

// Tests: what the frames show, one line per player ("name|level|health %|note").
std::string shownText();

} // namespace tlmp::hud
