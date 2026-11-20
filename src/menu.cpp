#include "menu.h"
#include "cegui.h"
#include "net/tailcat.h"
#include "log.h"

#include <windows.h>

#include <cstring>

namespace tlmp::menu {

using namespace cegui;

static Actions g_actions{};

void init(const Actions& actions)
{
  g_actions = actions;
  resolve();
}

// Our windows (looked up by name each frame: the title screen's windows come and go).
constexpr const char* kHost = "TLMP_HostButton";
constexpr const char* kJoin = "TLMP_JoinButton";
constexpr const char* kStatus = "TLMP_Status";
constexpr const char* kJoinAt = "TLMP_JoinAt";
constexpr const char* kCopy = "TLMP_CopyAddress";
constexpr const char* kCode = "TLMP_JoinCode";
constexpr const char* kCopyCode = "TLMP_CopyCode";
constexpr const char* kAddress = "TLMP_Address";
constexpr const char* kAddressBack = "TLMP_AddressBack";
constexpr const char* kConnect = "TLMP_Connect";
constexpr const char* kCancel = "TLMP_Cancel";
constexpr const char* kPaste = "TLMP_Paste";

static bool g_joining = false; // the address box is open

static void build(void* parent)
{
  // On the Continue button's row, in the two free spots to its left.
  make(parent, "GuiLook/StandardButton", kHost, "{{.5,-507},{1,-140}}", "{{0,246},{0,44}}", "Host Co-op");
  make(parent, "GuiLook/StandardButton", kJoin, "{{.5,-251},{1,-140}}", "{{0,246},{0,44}}", "Join Co-op");
  // Hosting: the address friends join at, big enough to read out.
  if (void* s = make(parent, "GuiLook/StaticText", kJoinAt, "{{.5,-497},{1,-238}}", "{{0,740},{0,40}}", ""))
  {
    prop(s, "Font", "SerifBig");
    prop(s, "TextColour", "FFFFC84A");
    prop(s, "MousePassThroughEnabled", "True");
  }
  // Next to it: copies the address for pasting to friends.
  if (void* b = make(parent, "GuiLook/StandardButton", kCopy, "{{.5,-150},{1,-242}}", "{{0,120},{0,40}}", "Copy IP"))
    api.setVisible(b, false);
  // Above it: the join code (too long to read out: copied and pasted).
  if (void* s = make(parent, "GuiLook/StaticText", kCode, "{{.5,-497},{1,-286}}", "{{0,740},{0,40}}", ""))
  {
    prop(s, "Font", "SerifBig");
    prop(s, "TextColour", "FFFFC84A");
    prop(s, "MousePassThroughEnabled", "True");
  }
  if (void* b = make(parent, "GuiLook/StandardButton", kCopyCode, "{{.5,-150},{1,-290}}", "{{0,120},{0,40}}",
                     "Copy code"))
    api.setVisible(b, false);
  if (void* s = make(parent, "GuiLook/StaticText", kStatus, "{{.5,-497},{1,-196}}", "{{0,740},{0,52}}", ""))
  {
    prop(s, "Font", "SerifSmall");
    prop(s, "TextColour", "FFFFFFFF");
    prop(s, "MousePassThroughEnabled", "True");
    prop(s, "HorzTextFormatting", "WordWrapLeftAligned");
  }
  // A framed field, as the character screen has its name box: a text panel under the edit box.
  // A little in from the left: the frame's border is drawn outside the window's rectangle.
  if (void* back = make(parent, "GuiLook/ItemText", kAddressBack, "{{.5,-494},{1,-140}}", "{{0,236},{0,44}}", ""))
  {
    prop(back, "MousePassThroughEnabled", "True");
    api.setVisible(back, false);
  }
  if (void* e = make(parent, "GuiLook/Editbox", kAddress, "{{.5,-484},{1,-134}}", "{{0,216},{0,32}}", ""))
  {
    prop(e, "MaxTextLength", "300"); // a join code is about 230 characters
    api.setVisible(e, false);
  }
  if (void* b = make(parent, "GuiLook/StandardButton", kConnect, "{{.5,-251},{1,-140}}", "{{0,120},{0,44}}", "Connect"))
    api.setVisible(b, false);
  if (void* b = make(parent, "GuiLook/StandardButton", kCancel, "{{.5,-126},{1,-140}}", "{{0,120},{0,44}}", "Cancel"))
    api.setVisible(b, false);
  if (void* b = make(parent, "GuiLook/StandardButton", kPaste, "{{.5,-1},{1,-140}}", "{{0,120},{0,44}}", "Paste"))
    api.setVisible(b, false);
  TLMP_LOG("menu: co-op buttons added to the title screen");
}

// Puts text on the Windows clipboard.
static bool copyToClipboard(const std::string& text)
{
  if (!OpenClipboard(nullptr))
    return false;
  EmptyClipboard();
  int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  HGLOBAL mem = n > 0 ? GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t)) : nullptr;
  bool ok = false;
  if (mem)
  {
    if (auto* w = static_cast<wchar_t*>(GlobalLock(mem)))
    {
      MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w, n);
      GlobalUnlock(mem);
      ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    }
    if (!ok)
      GlobalFree(mem);
  }
  CloseClipboard();
  return ok;
}

// The clipboard's text, cleaned up for an address box: surrounding spaces and line breaks go, and
// anything that can't be part of "host[:port]" ends it.
static std::string clipboardAddress()
{
  std::string out;
  if (!OpenClipboard(nullptr))
    return out;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (auto* w = h ? static_cast<const wchar_t*>(GlobalLock(h)) : nullptr)
  {
    const wchar_t* c = w;
    while (*c == L' ' || *c == L'\t' || *c == L'\r' || *c == L'\n')
      ++c;
    // A join code (letters, digits, '-', '_'), up to the first space or line break.
    std::string word;
    for (const wchar_t* k = c; *k && *k > L' ' && *k < 0x7F && word.size() < 1000; ++k)
      word += static_cast<char>(*k);
    if (net::tailcat::looksLikeCode(word))
    {
      GlobalUnlock(h);
      CloseClipboard();
      return word;
    }
    for (; *c && out.size() < 64; ++c)
    {
      bool ok = (*c >= L'0' && *c <= L'9') || (*c >= L'a' && *c <= L'z') || (*c >= L'A' && *c <= L'Z') || *c == L'.' ||
                *c == L':' || *c == L'-' || *c == L'[' || *c == L']';
      if (!ok)
        break;
      out += static_cast<char>(*c);
    }
    GlobalUnlock(h);
  }
  CloseClipboard();
  return out;
}

// Ctrl+V / Shift+Insert, pressed while this game is the window in front.
static bool pasteKeysPressed()
{
  static bool wasDown = false;
  DWORD pid = 0;
  HWND front = GetForegroundWindow();
  if (front)
    GetWindowThreadProcessId(front, &pid);
  bool down = pid == GetCurrentProcessId() &&
              (((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('V') & 0x8000)) ||
               ((GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_INSERT) & 0x8000)));
  bool pressed = down && !wasDown;
  wasDown = down;
  return pressed;
}

static void showJoinBox(bool show)
{
  g_joining = show;
  if (void* w = windowNamed(kHost))
    api.setVisible(w, !show);
  if (void* w = windowNamed(kJoin))
    api.setVisible(w, !show);
  if (void* w = windowNamed(kAddressBack))
    api.setVisible(w, show);
  if (void* w = windowNamed(kAddress))
  {
    api.setVisible(w, show);
    if (show)
    {
      text(w, g_actions.lastAddress ? g_actions.lastAddress() : "");
      if (api.activate)
        api.activate(w);
    }
  }
  if (void* w = windowNamed(kConnect))
    api.setVisible(w, show);
  if (void* w = windowNamed(kCancel))
    api.setVisible(w, show);
  if (void* w = windowNamed(kPaste))
    api.setVisible(w, show);
}

void tick()
{
  if (!api.ok)
    return;
  void* system = api.system();
  void* sheet = system ? api.sheet(system) : nullptr;
  if (!sheet)
    return;
  // The title screen: its Continue button is showing.
  Str cont("ContinueLast");
  void* continueButton = api.search(sheet, cont.p());
  if (!continueButton || !api.visible(continueButton, false))
    return;
  void* parent = api.parent(continueButton);
  if (!parent)
    return;
  void* host = windowNamed(kHost);
  if (!host)
  {
    build(parent);
    g_joining = false;
    host = windowNamed(kHost);
    if (!host)
      return;
  }

  bool inSession = g_actions.inSession && g_actions.inSession();
  if (void* s = windowNamed(kStatus))
  {
    static std::string last;
    std::string now = g_joining ? "Co-op: paste (Ctrl+V) the host's join code, or type their address (IP, or "
                                  "IP:port), then Connect"
                                : g_actions.status ? g_actions.status() : "";
    if (now != last)
    {
      last = now;
      text(s, now);
    }
  }
  std::string address = g_actions.joinAddress ? g_actions.joinAddress() : "";
  if (void* s = windowNamed(kJoinAt))
  {
    static std::string last;
    std::string now = address.empty() || g_joining ? "" : "Friends join at " + address;
    if (now != last)
    {
      last = now;
      text(s, now);
    }
  }
  static bool copyDown = false;
  static DWORD copiedUntil = 0;
  if (void* copy = windowNamed(kCopy))
  {
    bool show = !address.empty() && !g_joining;
    if (api.visible(copy, false) != show)
      api.setVisible(copy, show);
    if (clicked(copy, copyDown) && copyToClipboard(address))
    {
      text(copy, "Copied!");
      copiedUntil = GetTickCount() + 2000;
      TLMP_LOG("menu: copied the join address to the clipboard");
    }
    if (copiedUntil && GetTickCount() >= copiedUntil)
    {
      copiedUntil = 0;
      text(copy, "Copy IP");
    }
  }
  // The join code: its state, and Copy once there is one.
  std::string code = g_actions.joinCode ? g_actions.joinCode() : "";
  if (void* s = windowNamed(kCode))
  {
    static std::string last;
    std::string state = g_actions.codeStatus ? g_actions.codeStatus() : "";
    std::string now = g_joining ? "" : state == "Join code ready" ? "Join code ready: copy it for your friends" : state;
    if (now != last)
    {
      last = now;
      text(s, now);
    }
  }
  static bool copyCodeDown = false;
  static DWORD codeCopiedUntil = 0;
  if (void* copy = windowNamed(kCopyCode))
  {
    bool show = !code.empty() && !g_joining;
    if (api.visible(copy, false) != show)
      api.setVisible(copy, show);
    if (clicked(copy, copyCodeDown) && copyToClipboard(code))
    {
      text(copy, "Copied!");
      codeCopiedUntil = GetTickCount() + 2000;
      TLMP_LOG("menu: copied the join code to the clipboard");
    }
    if (codeCopiedUntil && GetTickCount() >= codeCopiedUntil)
    {
      codeCopiedUntil = 0;
      text(copy, "Copy code");
    }
  }
  if (!g_joining)
  {
    static bool shownInSession = false;
    if (inSession != shownInSession)
    {
      shownInSession = inSession;
      text(host, inSession ? "Leave Co-op" : "Host Co-op");
      if (void* j = windowNamed(kJoin))
        api.setVisible(j, !inSession);
    }
  }

  static bool hostDown = false, joinDown = false, connectDown = false, cancelDown = false;
  if (clicked(host, hostDown))
  {
    if (inSession)
      g_actions.leave();
    else
      g_actions.host();
  }
  if (clicked(windowNamed(kJoin), joinDown))
    showJoinBox(true);
  if (clicked(windowNamed(kCancel), cancelDown))
    showJoinBox(false);
  // Pasting (the game's edit box has no clipboard of its own): the address replaces what's there.
  static bool pasteDown = false;
  bool pasteClicked = clicked(windowNamed(kPaste), pasteDown);
  if (g_joining && (pasteClicked || pasteKeysPressed()))
  {
    std::string pasted = clipboardAddress();
    if (void* box = windowNamed(kAddress); box && !pasted.empty())
    {
      text(box, pasted);
      prop(box, "CaratIndex", std::to_string(pasted.size()));
      TLMP_LOG("menu: pasted an address into the join box");
    }
  }
  if (clicked(windowNamed(kConnect), connectDown))
  {
    std::string typed = textOf(windowNamed(kAddress));
    showJoinBox(false);
    if (!typed.empty())
      g_actions.join(typed);
  }
}

} // namespace tlmp::menu
