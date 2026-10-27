#pragma once

#include <windows.h>

namespace tlmp::crashlog {

void install(HMODULE self);

// Set while TLMP itself is probing memory under SEH, so expected faults aren't logged.
extern volatile long g_probing;
void uninstall();

} // namespace tlmp::crashlog
