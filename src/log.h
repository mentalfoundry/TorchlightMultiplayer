#pragma once

// Minimal thread-safe logger writing to TLMP_logs\TLMP_<pid>.log next to Torchlight.exe.
// We deliberately do not use Ogre's LogManager: its API takes VS2008 std::string,
// which is ABI-incompatible with anything a modern compiler produces.
namespace tlmp::log {

void open();
void close();
void write(const char* fmt, ...);

} // namespace tlmp::log

#define TLMP_LOG(...) ::tlmp::log::write(__VA_ARGS__)
