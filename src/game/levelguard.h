#pragma once

// A guard against units freed while still in a level's lists (the rare crash in the unit activity
// pass, re/NOTES.md "Open"). Characters that are freed are remembered with who freed them; before
// the level's once-a-second activity pass walks its unit and item lists, every entry is checked
// (a live unit's first word is its vtable, in the game's read-only data). A dead entry is logged --
// what it was, who freed it -- and unlinked, so the game goes on instead of crashing.
namespace tlmp::game::levelguard {

// CCharacter destructor hook: `callers` are the return addresses above it. Units on the dying
// unit's level (and the current one) that have it as their master (+0x5B4) are let go of it.
void noteDestroyed(void* unit, void* const* callers, int count, void* currentLevel);
// Activity pass hook (0x4F7340), before the game's own: check (and repair) the level's lists.
void checkLists(void* level);
// A unit's own update is about to use its level resources (+0x60): false if they belong to another
// (freed) level -- logged once per unit; the caller then skips what would crash.
bool unitResourcesOk(void* unit, void* level);
// Names our own units for the logs (" -- slot 1's stand-in" etc.).
using DescribeFn = const char* (*)(void* unit);
void setDescriber(DescribeFn fn);

} // namespace tlmp::game::levelguard
