// Logs access violations (with the faulting address and likely call stack) to the TLMP log.
// Uses a vectored handler so it runs before the game's own unhandled-exception filter;
// it never handles the exception, only records it.

#include "crashlog.h"
#include "log.h"

#include <windows.h>

namespace tlmp::crashlog {

constexpr uintptr_t kTextBegin = 0x00401000; // Torchlight.exe .text
constexpr uintptr_t kTextEnd = 0x00A843CB;

volatile long g_probing = 0;
static PVOID g_handler = nullptr;
static HMODULE g_self = nullptr;

static const char* moduleOf(uintptr_t addr, uintptr_t& base)
{
  static char name[MAX_PATH];
  HMODULE mod = nullptr;
  base = 0;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(addr), &mod))
    return "?";
  base = reinterpret_cast<uintptr_t>(mod);
  GetModuleFileNameA(mod, name, MAX_PATH);
  const char* slash = strrchr(name, '\\');
  return slash ? slash + 1 : name;
}

static LONG CALLBACK onException(EXCEPTION_POINTERS* ep)
{
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_probing)
    return EXCEPTION_CONTINUE_SEARCH;

  const CONTEXT* c = ep->ContextRecord;
  uintptr_t base;
  const char* mod = moduleOf(c->Eip, base);
  TLMP_LOG("CRASH: access violation (%s %08lx) at %08lx (%s+%lx)", er->ExceptionInformation[0] ? "write" : "read",
           static_cast<unsigned long>(er->ExceptionInformation[1]), c->Eip, mod, c->Eip - base);
  TLMP_LOG("CRASH: eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx", c->Eax, c->Ebx,
           c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);

  // Heuristic stack walk: any stack dword that points into Torchlight.exe .text or into
  // TLMP.dll is a probable return address.
  auto* sp = reinterpret_cast<const uintptr_t*>(c->Esp);
  int found = 0;
  for (int i = 0; i < 2048 && found < 24; i++)
  {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
      break;
    uintptr_t v = sp[i];
    bool inGame = v >= kTextBegin && v < kTextEnd;
    uintptr_t modBase = 0;
    bool inSelf = !inGame && g_self && moduleOf(v, modBase) && modBase == reinterpret_cast<uintptr_t>(g_self);
    if (inGame || inSelf)
    {
      TLMP_LOG("CRASH:   [esp+%03x] %08lx%s", i * 4, static_cast<unsigned long>(v), inSelf ? " (TLMP.dll)" : "");
      found++;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void install(HMODULE self)
{
  g_self = self;
  if (!g_handler)
    g_handler = AddVectoredExceptionHandler(1, onException);
}

void uninstall()
{
  if (g_handler)
    RemoveVectoredExceptionHandler(g_handler);
  g_handler = nullptr;
}

} // namespace tlmp::crashlog
