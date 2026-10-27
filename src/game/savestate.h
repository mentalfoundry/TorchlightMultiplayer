#pragma once

#include <cstdint>
#include <vector>

// The game's own save states (engine.h: items 0x1B0, characters 0x278) as bytes, for sending a
// unit to another machine exactly as a save would keep it (affixes, champion name and bonuses,
// equipment, health). The states read and write a FILE* of the game's CRT (msvcr90), so they go
// through a temp file opened with that CRT's functions.
namespace tlmp::game::savestate {

using WriteFn = void(__thiscall*)(void* state, void* file);
using ReadFn = void(__thiscall*)(void* state, void* file, uint32_t version);

bool available();
// A new, empty state of `size` bytes built with the game's ctor (Ogre allocator, as the game does).
void* create(uint32_t size, void*(__thiscall* ctor)(void*));
void destroy(void* state); // vtable[0](1)
bool toBytes(void* state, WriteFn write, std::vector<uint8_t>& out);
bool fromBytes(void* state, ReadFn read, uint32_t version, const std::vector<uint8_t>& bytes);

} // namespace tlmp::game::savestate
