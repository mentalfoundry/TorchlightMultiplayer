#pragma once

#include <cstdint>
#include <string>

namespace tlmp {

enum class NetMode
{
  Off,
  Host,
  Client,
};

struct Config
{
  NetMode mode = NetMode::Off;
  std::string address = "127.0.0.1"; // client: host to connect to
  uint16_t port = 27960;
  std::string playerName = "Adventurer";
  float healthPerPlayer = 0.5f; // host: monster health added per player beyond the first ([party])
  uint32_t devWorldSeed = 0; // -tlmp-worldseed N: act as if a host sent N (testing only)
  std::wstring repairPetType;  // -tlmp-repairpet <Type> <Name>: give a pet-less character its pet back
  std::wstring repairPetName;
  std::wstring testScript;     // -tlmp-test <file>: run a test scenario (game/autotest.cpp)
  bool devMirror = false;    // -tlmp-mirror: show our own character as remote slot 3 (testing only)
};

// Loads TLMP.ini from the game directory, then applies command-line overrides:
//   -tlmp-host [port]
//   -tlmp-join <address>[:port]
//   -tlmp-name <name>
// The command line wins so several instances can be started from one install.
Config loadConfig();

const char* toString(NetMode mode);

} // namespace tlmp
