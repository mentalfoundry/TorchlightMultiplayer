#pragma once

namespace tlmp::profile {

// Called from DllMain. No-op unless the command line has "-tlmp-profile <name>".
void installEarly();

// Redirected AppData directory, or nullptr when running with the normal profile.
const wchar_t* activeProfileDir();

} // namespace tlmp::profile
