// OgreGUIRenderer.dll proxy. Every export is forwarded to the original (renamed to
// OgreGUIRenderer_orig.dll by install.ps1) through OgreGUIRenderer.def.
//
// Its only job is to make TLMP.dll a load-time dependency of Torchlight.exe, so TLMP's
// DllMain runs during process startup -- after the loader has resolved Torchlight's
// imports but before WinMain reads settings.txt. That is required for profile
// redirection; everything else in TLMP starts later from dllStartPlugin.

#include <windows.h>

extern "C" __declspec(dllimport) void __cdecl tlmpEarlyAnchor();

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
  if (reason == DLL_PROCESS_ATTACH)
  {
    DisableThreadLibraryCalls(inst);
    tlmpEarlyAnchor();
  }
  return TRUE;
}
