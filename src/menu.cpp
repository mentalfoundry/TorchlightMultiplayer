#include "menu.h"
#include "log.h"

#include <windows.h>

#include <cstring>

namespace tlmp::menu {

// CEGUIBase exports (CEGUI 0.6, MSVC2008 ABI). Window derives from PropertySet first, so a
// Window* is also its PropertySet*.
struct Cegui
{
  void* (*system)() = nullptr;
  void*(__thiscall* sheet)(void* system) = nullptr;
  void*(__thiscall* search)(const void* window, const void* name) = nullptr;
  void*(__thiscall* strCtor)(void* str, const unsigned char* utf8) = nullptr;
  void(__thiscall* strDtor)(void* str) = nullptr;
  const char*(__thiscall* cStr)(const void* str) = nullptr;
  void** windowManager = nullptr; // Singleton<WindowManager>::ms_Singleton
  void*(__thiscall* create)(void* wm, const void* type, const void* name, const void* prefix) = nullptr;
  bool(__thiscall* present)(const void* wm, const void* name) = nullptr;
  void*(__thiscall* getWindow)(const void* wm, const void* name) = nullptr;
  void(__thiscall* destroy)(void* wm, void* window) = nullptr;
  void(__thiscall* setProperty)(void* window, const void* name, const void* value) = nullptr;
  void(__thiscall* addChild)(void* parent, void* child) = nullptr;
  void(__thiscall* setText)(void* window, const void* text) = nullptr;
  const void*(__thiscall* getText)(const void* window) = nullptr;
  void(__thiscall* setVisible)(void* window, bool visible) = nullptr;
  bool(__thiscall* visible)(const void* window, bool localOnly) = nullptr;
  bool(__thiscall* pushed)(const void* button) = nullptr;
  bool(__thiscall* hovering)(const void* button) = nullptr;
  void*(__thiscall* parent)(const void* window) = nullptr;
  void(__thiscall* activate)(void* window) = nullptr;
  bool ok = false;
};

static Cegui g_c;
static Actions g_actions{};

// A CEGUI::String for the duration of a call.
struct Str
{
  alignas(8) unsigned char raw[256] = {};
  explicit Str(const std::string& s) { g_c.strCtor(raw, reinterpret_cast<const unsigned char*>(s.c_str())); }
  ~Str() { g_c.strDtor(raw); }
  const void* p() const { return raw; }
};

static void resolve()
{
  HMODULE m = GetModuleHandleW(L"CEGUIBase.dll");
  if (!m)
    return;
  auto get = [&](const char* n) { return GetProcAddress(m, n); };
  g_c.system = reinterpret_cast<void* (*)()>(get("?getSingleton@System@CEGUI@@SAAAV12@XZ"));
  g_c.sheet = reinterpret_cast<void*(__thiscall*)(void*)>(get("?getGUISheet@System@CEGUI@@QBEPAVWindow@2@XZ"));
  g_c.search = reinterpret_cast<void*(__thiscall*)(const void*, const void*)>(
    get("?recursiveChildSearch@Window@CEGUI@@QBEPAV12@ABVString@2@@Z"));
  g_c.strCtor = reinterpret_cast<void*(__thiscall*)(void*, const unsigned char*)>(get("??0String@CEGUI@@QAE@PBE@Z"));
  g_c.strDtor = reinterpret_cast<void(__thiscall*)(void*)>(get("??1String@CEGUI@@QAE@XZ"));
  g_c.cStr = reinterpret_cast<const char*(__thiscall*)(const void*)>(get("?c_str@String@CEGUI@@QBEPBDXZ"));
  g_c.windowManager = reinterpret_cast<void**>(get("?ms_Singleton@?$Singleton@VWindowManager@CEGUI@@@CEGUI@@1PAVWindowManager@2@A"));
  g_c.create = reinterpret_cast<void*(__thiscall*)(void*, const void*, const void*, const void*)>(
    get("?createWindow@WindowManager@CEGUI@@QAEPAVWindow@2@ABVString@2@00@Z"));
  g_c.present = reinterpret_cast<bool(__thiscall*)(const void*, const void*)>(
    get("?isWindowPresent@WindowManager@CEGUI@@QBE_NABVString@2@@Z"));
  g_c.getWindow = reinterpret_cast<void*(__thiscall*)(const void*, const void*)>(
    get("?getWindow@WindowManager@CEGUI@@QBEPAVWindow@2@ABVString@2@@Z"));
  g_c.destroy = reinterpret_cast<void(__thiscall*)(void*, void*)>(get("?destroyWindow@WindowManager@CEGUI@@QAEXPAVWindow@2@@Z"));
  g_c.setProperty = reinterpret_cast<void(__thiscall*)(void*, const void*, const void*)>(
    get("?setProperty@PropertySet@CEGUI@@QAEXABVString@2@0@Z"));
  g_c.addChild = reinterpret_cast<void(__thiscall*)(void*, void*)>(get("?addChildWindow@Window@CEGUI@@QAEXPAV12@@Z"));
  g_c.setText = reinterpret_cast<void(__thiscall*)(void*, const void*)>(get("?setText@Window@CEGUI@@QAEXABVString@2@@Z"));
  g_c.getText = reinterpret_cast<const void*(__thiscall*)(const void*)>(get("?getText@Window@CEGUI@@QBEABVString@2@XZ"));
  g_c.setVisible = reinterpret_cast<void(__thiscall*)(void*, bool)>(get("?setVisible@Window@CEGUI@@QAEX_N@Z"));
  g_c.visible = reinterpret_cast<bool(__thiscall*)(const void*, bool)>(get("?isVisible@Window@CEGUI@@QBE_N_N@Z"));
  g_c.pushed = reinterpret_cast<bool(__thiscall*)(const void*)>(get("?isPushed@ButtonBase@CEGUI@@QBE_NXZ"));
  g_c.hovering = reinterpret_cast<bool(__thiscall*)(const void*)>(get("?isHovering@ButtonBase@CEGUI@@QBE_NXZ"));
  g_c.parent = reinterpret_cast<void*(__thiscall*)(const void*)>(get("?getParent@Window@CEGUI@@QBEPAV12@XZ"));
  g_c.activate = reinterpret_cast<void(__thiscall*)(void*)>(get("?activate@Window@CEGUI@@QAEXXZ"));
  g_c.ok = g_c.system && g_c.sheet && g_c.search && g_c.strCtor && g_c.strDtor && g_c.cStr && g_c.windowManager &&
           g_c.create && g_c.present && g_c.getWindow && g_c.setProperty && g_c.addChild && g_c.setText &&
           g_c.getText && g_c.setVisible && g_c.visible && g_c.pushed && g_c.hovering && g_c.parent;
  if (!g_c.ok)
    TLMP_LOG("menu: CEGUI functions not found; no co-op buttons on the title screen");
}

void init(const Actions& actions)
{
  g_actions = actions;
  resolve();
}

static void* windowNamed(const char* name)
{
  void* wm = *g_c.windowManager;
  Str n(name);
  return wm && g_c.present(wm, n.p()) ? g_c.getWindow(wm, n.p()) : nullptr;
}

static void prop(void* w, const char* name, const std::string& value)
{
  Str n(name), v(value);
  g_c.setProperty(w, n.p(), v.p());
}

static void text(void* w, const std::string& value)
{
  Str v(value);
  g_c.setText(w, v.p());
}

static void* make(void* parent, const char* type, const char* name, const char* pos, const char* size,
                  const std::string& label)
{
  void* wm = *g_c.windowManager;
  Str t(type), n(name), prefix("");
  void* w = g_c.create(wm, t.p(), n.p(), prefix.p());
  if (!w)
    return nullptr;
  prop(w, "UnifiedPosition", pos);
  prop(w, "UnifiedSize", size);
  text(w, label);
  g_c.addChild(parent, w);
  return w;
}

// Our windows (looked up by name each frame: the title screen's windows come and go).
constexpr const char* kHost = "TLMP_HostButton";
constexpr const char* kJoin = "TLMP_JoinButton";
constexpr const char* kStatus = "TLMP_Status";
constexpr const char* kAddress = "TLMP_Address";
constexpr const char* kAddressBack = "TLMP_AddressBack";
constexpr const char* kConnect = "TLMP_Connect";
constexpr const char* kCancel = "TLMP_Cancel";

static bool g_joining = false; // the address box is open

static void build(void* parent)
{
  // On the Continue button's row, in the two free spots to its left.
  make(parent, "GuiLook/StandardButton", kHost, "{{.5,-507},{1,-140}}", "{{0,246},{0,44}}", "Host Co-op");
  make(parent, "GuiLook/StandardButton", kJoin, "{{.5,-251},{1,-140}}", "{{0,246},{0,44}}", "Join Co-op");
  if (void* s = make(parent, "GuiLook/StaticText", kStatus, "{{.5,-507},{1,-178}}", "{{0,750},{0,32}}", ""))
  {
    prop(s, "Font", "SerifSmall");
    prop(s, "TextColour", "FFFFFFFF");
    prop(s, "MousePassThroughEnabled", "True");
  }
  // A framed field, as the character screen has its name box: a text panel under the edit box.
  if (void* back = make(parent, "GuiLook/ItemText", kAddressBack, "{{.5,-507},{1,-140}}", "{{0,246},{0,44}}", ""))
  {
    prop(back, "MousePassThroughEnabled", "True");
    g_c.setVisible(back, false);
  }
  if (void* e = make(parent, "GuiLook/Editbox", kAddress, "{{.5,-497},{1,-134}}", "{{0,226},{0,32}}", ""))
  {
    prop(e, "MaxTextLength", "64");
    g_c.setVisible(e, false);
  }
  if (void* b = make(parent, "GuiLook/StandardButton", kConnect, "{{.5,-251},{1,-140}}", "{{0,120},{0,44}}", "Connect"))
    g_c.setVisible(b, false);
  if (void* b = make(parent, "GuiLook/StandardButton", kCancel, "{{.5,-126},{1,-140}}", "{{0,120},{0,44}}", "Cancel"))
    g_c.setVisible(b, false);
  TLMP_LOG("menu: co-op buttons added to the title screen");
}

// A click: the button was pushed and is now released. (Not checking that the mouse is still on
// it: the game feeds CEGUI the real cursor, which a click injected while the window is in the
// background doesn't move.)
static bool clicked(void* button, bool& wasPushed)
{
  if (!button || !g_c.visible(button, false))
  {
    wasPushed = false;
    return false;
  }
  bool now = g_c.pushed(button);
  bool click = wasPushed && !now;
  wasPushed = now;
  return click;
}

static std::string textOf(void* w)
{
  const void* s = w ? g_c.getText(w) : nullptr;
  const char* c = s ? g_c.cStr(s) : nullptr;
  return c ? c : "";
}

static void showJoinBox(bool show)
{
  g_joining = show;
  if (void* w = windowNamed(kHost))
    g_c.setVisible(w, !show);
  if (void* w = windowNamed(kJoin))
    g_c.setVisible(w, !show);
  if (void* w = windowNamed(kAddressBack))
    g_c.setVisible(w, show);
  if (void* w = windowNamed(kAddress))
  {
    g_c.setVisible(w, show);
    if (show)
    {
      text(w, g_actions.lastAddress ? g_actions.lastAddress() : "");
      if (g_c.activate)
        g_c.activate(w);
    }
  }
  if (void* w = windowNamed(kConnect))
    g_c.setVisible(w, show);
  if (void* w = windowNamed(kCancel))
    g_c.setVisible(w, show);
}

void tick()
{
  if (!g_c.ok)
    return;
  void* system = g_c.system();
  void* sheet = system ? g_c.sheet(system) : nullptr;
  if (!sheet)
    return;
  // The title screen: its Continue button is showing.
  Str cont("ContinueLast");
  void* continueButton = g_c.search(sheet, cont.p());
  if (!continueButton || !g_c.visible(continueButton, false))
    return;
  void* parent = g_c.parent(continueButton);
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
    std::string now = g_joining ? "Co-op: type the host's address (IP, or IP:port), then Connect"
                                : g_actions.status ? g_actions.status() : "";
    if (now != last)
    {
      last = now;
      text(s, now);
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
        g_c.setVisible(j, !inSession);
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
  if (clicked(windowNamed(kConnect), connectDown))
  {
    std::string address = textOf(windowNamed(kAddress));
    showJoinBox(false);
    if (!address.empty())
      g_actions.join(address);
  }
}

} // namespace tlmp::menu
