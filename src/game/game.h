#pragma once

#include "chat.h"

#include "net/protocol.h"

#include <cstdint>
#include <string>

// Bridge between the network session and Torchlight's objects. Everything here runs on
// the game's main thread (from the Ogre frame listener or from hooks).
namespace tlmp::game {

// PE TimeDateStamp of the GOG Torchlight.exe (build 1.7.8.70) these addresses target.
constexpr uint32_t kExpectedBuildStamp = 0x4FFC6815;

void tick(float dt);

// Level id of the floor currently loaded (0 if none).
uint32_t currentLevelId();

// Repair: if the local character has no pet, create one of this type and name.
void repairPet(const std::wstring& type, const std::wstring& name);

// Called every frame with whether this machine is hosting a connected session.
void setHostRole(bool isHost);

// Fills position/orientation/level of the local player. Returns false when there is no
// local player (startup); out.inGame is false on the title screen.
bool readLocalPlayerState(net::PlayerState& out);

// Class and pet of the local character; empty strings if there is none.
net::PlayerInfo localPlayerInfo();

// Chat. Typed in the chat box (chat.h); the console (opened with the game's console key) doubles
// as chat input: lines starting with "/" or "say " are sent to the other players instead of being
// executed. Messages show in the chat box and the console.
void setChatSender(void (*send)(const std::string& text));
void showChat(const std::string& who, const std::string& text, chat::Kind kind = chat::Kind::Player);
void say(const std::string& text); // to the others, and shown here
// Name to show for a remote player: their character's name once known, else `fallback`
// (the network name they joined with).
std::string displayName(uint8_t slot, const std::string& fallback);
// Called from the CConsole::Execute hook; returns true if the line was chat (consumed).
bool onConsoleLine(const wchar_t* line);
// Session start/end: turns the console (and "console doesn't pause") on in memory and
// blocks cheat commands; endSession restores the player's own settings.
void enableConsoleForSession();
void endSession();
// Client: tells the player the host has gone (the session ended; this game goes on alone).
void showHostLeft();
// A message for the player: on screen for `seconds`, and in the chat (console) history.
void showMessage(const std::string& text, float seconds);
// In a game (not on the title screen or loading).
bool playing();

// Bracket the game's settings writer (0x5C5530) so the player's own console settings are
// what gets saved.
void beforeSettingsWrite(void* settings);
void afterSettingsWrite(void* settings);

void onRemoteJoined(uint8_t slot, const std::string& name);
void onRemoteLeft(uint8_t slot);
void onRemoteState(uint8_t slot, const net::PlayerState& state);
void onRemoteInfo(uint8_t slot, const net::PlayerInfo& info);

// Called from the CLevel destructor hook, before the level frees its units.
void onLevelDestroyed(void* level);
// CCharacter destructor hook: forget a stand-in or pet copy that is being freed.
void onUnitDestroyed(void* unit);
// For logs: " -- slot 1's stand-in" and the like for units of ours, "" for the game's own.
const char* describeUnit(void* unit);

// Called from the RNG hook at the level layout seed call (0x4FF6DC). Returns the seed to
// actually use: with a host world seed active, the character's own world-seed term is
// swapped for the host's.
uint32_t onLevelLayoutSeeded(void* level, uint32_t seed);

// Host: the local character's world seed, announced to clients. 0 if unknown.
uint32_t localWorldSeed();

// Client: adopt the host's world seed (0 = host gone, restore our own).
void onHostWorldSeed(uint32_t seed);

// Bracket the game's character save so the save file keeps the character's own world
// seed rather than the host's.
void beforeSave(void* client);
void afterSave(void* client);

} // namespace tlmp::game
