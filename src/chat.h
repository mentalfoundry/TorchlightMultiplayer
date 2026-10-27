#pragma once

#include <cstdint>
#include <string>

// The in-game chat box: the last messages on the left of the screen (fading out a while after
// they arrive), and an input line opened with Enter -- Enter sends, Esc closes it. While it is
// open the game gets no key presses (its hotkeys would fire on every letter typed): the game's
// key manager is fed from the window procedure after CEGUI (0x40DE20), and our hook on it
// (CKeyManager::OnMessage, 0x4E7120) keeps the key downs from it; key ups still go through, so no
// key is left held.
namespace tlmp::chat {

enum class Kind
{
  Player, // someone else
  Own,    // what we said
  System, // TLMP's own messages (joins, leaves ...)
};

struct Actions
{
  void (*send)(const std::string& text); // say it (to the others, and shown here)
  bool (*canChat)();                     // in gameplay with a session on
};

void init(const Actions& actions);
void add(const std::string& who, const std::string& text, Kind kind);
void tick(); // every frame, main thread

// CKeyManager::OnMessage (window message, virtual key): true keeps it from the game.
bool onKey(uint32_t msg, uint32_t vk);
bool typing(); // the input line is open

// Tests: what the box shows (rows, oldest first, '\n'-separated).
std::string shownText();

} // namespace tlmp::chat
