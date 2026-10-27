#include "game/autotest.h"
#include "game/anim.h"
#include "game/diag.h"
#include "game/engine.h"
#include "game/floor.h"
#include "game/hooks.h"
#include "game/internal.h"
#include "game/loot.h"
#include "game/monsters.h"
#include "game/party.h"
#include "game/minions.h"
#include "game/skills.h"
#include "game/spawns.h"
#include "game/triggers.h"
#include "log.h"
#include "net/session.h"
#include "net/upnp.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace tlmp::game::autotest {

using net::Msg;

namespace {

constexpr float kDefaultTimeout = 60.0f;
constexpr float kExpectTimeout = 15.0f;
constexpr float kReach = 30.0f; // "nearest monster" must be this close

struct Step
{
  std::vector<std::string> args;
  int line = 0;
  float timeout = kDefaultTimeout;
};

enum class Poll
{
  Wait,
  Done,
  Fail,
};

net::Session* g_session = nullptr;
bool g_active = false;
std::vector<Step> g_steps;
size_t g_pc = 0;
bool g_begun = false;
float g_stepStart = 0.0f;
float g_lastAction = 0.0f;
std::string g_failure;
std::wstring g_outDir;
std::string g_profile;
HWND g_window = nullptr;
bool g_placed = false;
unsigned g_passed = 0, g_failed = 0;

std::set<std::string> g_syncSeen[net::kMaxPlayers];
std::vector<std::string> g_notices; // every notice shown, in order
size_t g_noticeMark = 0;
int g_xpMark = 0;
float g_healthMark = 0.0f;
uint32_t g_levelMark = 0;
unsigned g_monstersMark = 0;
int g_groundMark = 0;
unsigned g_rollsMark = 0;
unsigned g_castsSentMark = 0, g_castsPlayedMark = 0;
void* g_area = nullptr; // trigger area of the last `teleport area`
bool g_hasShared = false; // a position another machine shared (`share node`), for `teleport shared`
float g_sharedX = 0.0f, g_sharedZ = 0.0f;

// Items lying on this floor within `radius` of the local player (the level's ground list, +0x68).
int groundItemsNear(float radius)
{
  engine::Level* level = currentLevel();
  auto* m = reinterpret_cast<engine::Unit*>(localPlayer());
  if (!level || !m)
    return 0;
  Vec3 at = unitPosition(m);
  uint32_t list = 0, node = 0;
  int n = 0;
  if (!diag::readU32(reinterpret_cast<uintptr_t>(level) + 0x68, list) || !list || !diag::readU32(list, node))
    return 0;
  for (int guard = 0; node && guard < 20000; guard++)
  {
    uint32_t item = 0, next = 0;
    diag::readU32(node, item);
    diag::readU32(node + 4, next);
    if (item && distXZ(unitPosition(reinterpret_cast<void*>(item)), at) <= radius)
      n++;
    node = next;
  }
  return n;
}
int g_itemsMark = 0;

// step-local state
Vec3 g_target{};
engine::Unit* g_targetUnit = nullptr;
uint32_t g_sharedKey = 0;

// --- output ------------------------------------------------------------------------------------

void result(const char* status, const Step& s, const std::string& detail)
{
  std::string cmd;
  for (const auto& a : s.args)
    cmd += (cmd.empty() ? "" : " ") + a;
  char line[1024];
  std::snprintf(line, sizeof(line), "%s line %d: %s%s%s", status, s.line, cmd.c_str(), detail.empty() ? "" : " -- ",
                detail.c_str());
  TLMP_LOG("test: %s", line);
  std::ofstream f(g_outDir + L"\\results.txt", std::ios::app);
  f << line << "\n";
}

std::string jsonEscape(const std::string& s)
{
  std::string o;
  for (char c : s)
  {
    if (c == '"' || c == '\\')
      o += '\\';
    if (static_cast<unsigned char>(c) >= 0x20)
      o += c;
  }
  return o;
}

std::string unitName(const void* unit)
{
  wchar_t buf[64];
  if (!unit || !diag::readWStringAny(reinterpret_cast<uintptr_t>(unit) + engine::kUnitName, buf, 64))
    return {};
  return toUtf8(buf);
}

// --- the game window -----------------------------------------------------------------------------

BOOL CALLBACK findWindow(HWND hwnd, LPARAM)
{
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr)
  {
    g_window = hwnd;
    return FALSE;
  }
  return TRUE;
}

void placeWindow()
{
  if (g_placed)
    return;
  if (!g_window)
    EnumWindows(findWindow, 0);
  if (!g_window)
    return;
  g_placed = true;
  // Host on the left, clients to the right of it, so both can be seen (and screenshotted) at once.
  int index = g_profile.empty() ? 0 : (g_profile.back() - '1');
  if (index < 0 || index > 3)
    index = 0;
  SetWindowPos(g_window, HWND_TOP, 10 + index * 830, 30, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

bool screenshot(const std::string& tag)
{
  if (!g_window)
    return false;
  RECT rc;
  GetClientRect(g_window, &rc);
  POINT origin{0, 0};
  ClientToScreen(g_window, &origin);
  int w = rc.right, h = rc.bottom;
  if (w <= 0 || h <= 0)
    return false;
  // PrintWindow with PW_RENDERFULLCONTENT renders the window (Direct3D content included) even when
  // other windows cover it; the client area is then cut out of the whole-window image.
  RECT wr;
  GetWindowRect(g_window, &wr);
  int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
  HDC screen = GetDC(nullptr);
  HDC full = CreateCompatibleDC(screen);
  HBITMAP fullBmp = CreateCompatibleBitmap(screen, ww, wh);
  HGDIOBJ oldFull = SelectObject(full, fullBmp);
  PrintWindow(g_window, full, 2 /* PW_RENDERFULLCONTENT */);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, h, full, origin.x - wr.left, origin.y - wr.top, SRCCOPY);
  SelectObject(mem, old);
  SelectObject(full, oldFull);
  DeleteObject(fullBmp);
  DeleteDC(full);
  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = w;
  bi.biHeight = h;
  bi.biPlanes = 1;
  bi.biBitCount = 24;
  bi.biCompression = BI_RGB;
  int stride = ((w * 3 + 3) / 4) * 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(stride) * h);
  GetDIBits(mem, bmp, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  BITMAPFILEHEADER bf{};
  bf.bfType = 0x4D42;
  bf.bfOffBits = sizeof(bf) + sizeof(bi);
  bf.bfSize = bf.bfOffBits + static_cast<DWORD>(pixels.size());
  std::ofstream f(g_outDir + L"\\shot_" + fromUtf8(tag) + L".bmp", std::ios::binary);
  f.write(reinterpret_cast<const char*>(&bf), sizeof(bf));
  f.write(reinterpret_cast<const char*>(&bi), sizeof(bi));
  f.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  return true;
}

// --- CEGUI (clicking buttons the way a mouse would) ------------------------------------------------

struct Cegui
{
  void* (*system)() = nullptr;
  void*(__thiscall* sheet)(void* system) = nullptr;
  void*(__thiscall* search)(const void* window, const void* name) = nullptr;
  bool(__thiscall* visible)(const void* window, bool localOnly) = nullptr;
  void*(__thiscall* pixelRect)(const void* window, void* out) = nullptr;
  bool(__thiscall* mousePos)(void* system, float x, float y) = nullptr;
  bool(__thiscall* mouseDown)(void* system, int button) = nullptr;
  bool(__thiscall* mouseUp)(void* system, int button) = nullptr;
  void*(__thiscall* strCtor)(void* str, const unsigned char* utf8) = nullptr;
  void(__thiscall* strDtor)(void* str) = nullptr;
  void*(__thiscall* unclippedRect)(const void* window, void* out) = nullptr;
  void*(__thiscall* parent)(const void* window) = nullptr;
  void*(__thiscall* childAt)(const void* window, const float* pos) = nullptr;
  bool ok = false;
};

const Cegui& cegui()
{
  static Cegui c = [] {
    Cegui r;
    HMODULE m = GetModuleHandleW(L"CEGUIBase.dll");
    if (!m)
      return r;
    auto get = [&](const char* n) { return GetProcAddress(m, n); };
    r.system = reinterpret_cast<void* (*)()>(get("?getSingleton@System@CEGUI@@SAAAV12@XZ"));
    r.sheet = reinterpret_cast<void*(__thiscall*)(void*)>(get("?getGUISheet@System@CEGUI@@QBEPAVWindow@2@XZ"));
    r.search = reinterpret_cast<void*(__thiscall*)(const void*, const void*)>(
      get("?recursiveChildSearch@Window@CEGUI@@QBEPAV12@ABVString@2@@Z"));
    r.visible = reinterpret_cast<bool(__thiscall*)(const void*, bool)>(get("?isVisible@Window@CEGUI@@QBE_N_N@Z"));
    r.pixelRect = reinterpret_cast<void*(__thiscall*)(const void*, void*)>(get("?getPixelRect@Window@CEGUI@@QBE?AVRect@2@XZ"));
    r.mousePos = reinterpret_cast<bool(__thiscall*)(void*, float, float)>(get("?injectMousePosition@System@CEGUI@@QAE_NMM@Z"));
    r.mouseDown =
      reinterpret_cast<bool(__thiscall*)(void*, int)>(get("?injectMouseButtonDown@System@CEGUI@@QAE_NW4MouseButton@2@@Z"));
    r.mouseUp =
      reinterpret_cast<bool(__thiscall*)(void*, int)>(get("?injectMouseButtonUp@System@CEGUI@@QAE_NW4MouseButton@2@@Z"));
    r.strCtor = reinterpret_cast<void*(__thiscall*)(void*, const unsigned char*)>(get("??0String@CEGUI@@QAE@PBE@Z"));
    r.strDtor = reinterpret_cast<void(__thiscall*)(void*)>(get("??1String@CEGUI@@QAE@XZ"));
    r.unclippedRect =
      reinterpret_cast<void*(__thiscall*)(const void*, void*)>(get("?getUnclippedPixelRect@Window@CEGUI@@QBE?AVRect@2@XZ"));
    r.parent = reinterpret_cast<void*(__thiscall*)(const void*)>(get("?getParent@Window@CEGUI@@QBEPAV12@XZ"));
    r.childAt = reinterpret_cast<void*(__thiscall*)(const void*, const float*)>(
      get("?getTargetChildAtPosition@Window@CEGUI@@QBEPAV12@ABVVector2@2@@Z"));
    r.ok = r.system && r.sheet && r.search && r.visible && r.pixelRect && r.mousePos && r.mouseDown && r.mouseUp &&
           r.strCtor && r.strDtor;
    return r;
  }();
  return c;
}

// Clicks the named window if it is showing; false if it isn't (yet). pressOnly: the button goes
// down now and up with releaseMouse() (a click spanning frames, as a real one does).
bool clickWindow(const std::string& name, bool pressOnly = false)
{
  const Cegui& c = cegui();
  if (!c.ok)
    return false;
  void* system = c.system();
  void* sheet = system ? c.sheet(system) : nullptr;
  if (!sheet)
    return false;
  alignas(8) unsigned char str[256] = {};
  c.strCtor(str, reinterpret_cast<const unsigned char*>(name.c_str()));
  void* window = c.search(sheet, str);
  c.strDtor(str);
  if (!window || !c.visible(window, false))
    return false;
  float rect[4] = {}; // CEGUI::Rect is { top, bottom, left, right }
  c.pixelRect(window, rect);
  float x = (rect[2] + rect[3]) * 0.5f, y = (rect[0] + rect[1]) * 0.5f;
  // Only click what is really under that point (menus slide in; a hidden copy may share the name).
  float at[2] = {x, y};
  void* hit = c.childAt ? c.childAt(sheet, at) : window;
  bool onTarget = false;
  for (void* w = hit; w && c.parent; w = c.parent(w))
    if (w == window)
      onTarget = true;
  if (!onTarget)
  {
    static float nextLog = 0.0f;
    if (clock() >= nextLog)
    {
      nextLog = clock() + 3.0f;
      float un[4] = {}, sh[4] = {};
      if (c.unclippedRect)
        c.unclippedRect(window, un);
      c.pixelRect(sheet, sh);
      TLMP_LOG("test: '%s' not clickable yet: x %.0f-%.0f y %.0f-%.0f (unclipped x %.0f-%.0f y %.0f-%.0f), "
               "screen %.0fx%.0f, something else %p is there",
               name.c_str(), rect[2], rect[3], rect[0], rect[1], un[2], un[3], un[0], un[1], sh[3], sh[1], hit);
    }
    return false;
  }
  c.mousePos(system, x, y);
  c.mouseDown(system, 0);
  if (!pressOnly)
    c.mouseUp(system, 0);
  TLMP_LOG("test: clicked '%s' at (%.0f, %.0f)", name.c_str(), x, y);
  return true;
}

void releaseMouse()
{
  const Cegui& c = cegui();
  if (void* system = c.ok ? c.system() : nullptr)
    c.mouseUp(system, 0);
}

// Sets a window's text (an edit box's contents, as typing would leave them).
bool setWindowText(const std::string& name, const std::string& value)
{
  const Cegui& c = cegui();
  static auto setText = reinterpret_cast<void(__thiscall*)(void*, const void*)>(
    GetProcAddress(GetModuleHandleW(L"CEGUIBase.dll"), "?setText@Window@CEGUI@@QAEXABVString@2@@Z"));
  void* system = c.ok ? c.system() : nullptr;
  void* sheet = system ? c.sheet(system) : nullptr;
  if (!sheet || !setText)
    return false;
  alignas(8) unsigned char str[256] = {}, val[256] = {};
  c.strCtor(str, reinterpret_cast<const unsigned char*>(name.c_str()));
  void* window = c.search(sheet, str);
  c.strDtor(str);
  if (!window || !c.visible(window, false))
    return false;
  c.strCtor(val, reinterpret_cast<const unsigned char*>(value.c_str()));
  setText(window, val);
  c.strDtor(val);
  return true;
}

// --- game state helpers ---------------------------------------------------------------------------

engine::Unit* me()
{
  return reinterpret_cast<engine::Unit*>(localPlayer());
}

int myXp()
{
  return me() ? engine::field<int>(me(), 0x3C8) : 0; // GiveExperience adds to +0x3C8
}

bool isDeadUnit(const void* u)
{
  int ai = engine::field<int>(u, engine::kUnitAIState);
  return ai == engine::kAIStateDying || ai == engine::kAIStateDead;
}

int backpackItems()
{
  void* inv = me() ? engine::field<void*>(me(), engine::kUnitInventory) : nullptr;
  if (!inv)
    return 0;
  auto** entries = engine::field<uint8_t**>(inv, 0x1C);
  uint32_t count = engine::field<uint32_t>(inv, 0x20);
  int n = 0;
  for (uint32_t i = 0; entries && i < count && i < 1000; i++)
    if (entries[i] && engine::field<void*>(entries[i], 8) && engine::field<int>(entries[i], 0xC) >= net::kEquipSlots)
      n++;
  return n;
}

void* backpackEquipment()
{
  void* inv = me() ? engine::field<void*>(me(), engine::kUnitInventory) : nullptr;
  if (!inv)
    return nullptr;
  auto** entries = engine::field<uint8_t**>(inv, 0x1C);
  uint32_t count = engine::field<uint32_t>(inv, 0x20);
  for (uint32_t i = 0; entries && i < count && i < 1000; i++)
  {
    if (!entries[i])
      continue;
    void* item = engine::field<void*>(entries[i], 8);
    int slot = engine::field<int>(entries[i], 0xC);
    const char* cls = item ? diag::rttiName(reinterpret_cast<uintptr_t>(item)) : nullptr;
    if (slot >= net::kEquipSlots && cls && std::strcmp(cls, ".?AVCEquipment@@") == 0)
      return item;
  }
  return nullptr;
}

struct NearestCtx
{
  Vec3 from;
  float best;
  engine::Unit* unit;
  unsigned alive;
};

engine::Unit* nearestMonster(float within, unsigned* aliveCount = nullptr)
{
  NearestCtx ctx{me() ? unitPosition(me()) : Vec3{}, within, nullptr, 0};
  monsters::forEachKnown(
    [](uint32_t, engine::Unit* u, bool, bool dead, void* p) {
      auto* c = static_cast<NearestCtx*>(p);
      if (dead)
        return;
      c->alive++;
      float d = distXZ(unitPosition(u), c->from);
      if (d < c->best)
      {
        c->best = d;
        c->unit = u;
      }
    },
    &ctx);
  if (aliveCount)
    *aliveCount = ctx.alive;
  return ctx.unit;
}

bool knownDead(const engine::Unit* target)
{
  struct C
  {
    const engine::Unit* t;
    bool found, dead;
  } c{target, false, false};
  monsters::forEachKnown(
    [](uint32_t, engine::Unit* u, bool, bool dead, void* p) {
      auto* c = static_cast<C*>(p);
      if (u == c->t)
      {
        c->found = true;
        c->dead = dead;
      }
    },
    &c);
  return !c.found || c.dead;
}

void changeLevel(const wchar_t* dungeon, int mode)
{
  void* client = reinterpret_cast<void*>(gameClient());
  if (!client)
    return;
  auto fn = reinterpret_cast<engine::ChangeLevelFn>(engine::kChangeLevel); // through our hook, like the game
  fn(client, vs::make(dungeon), mode, 0, 0, vs::make(L""), 0);
}

int intArg(const Step& s, size_t i, int fallback = 0)
{
  return i < s.args.size() ? std::atoi(s.args[i].c_str()) : fallback;
}

float floatArg(const Step& s, size_t i, float fallback = 0.0f)
{
  return i < s.args.size() ? static_cast<float>(std::atof(s.args[i].c_str())) : fallback;
}

std::string restFrom(const Step& s, size_t i)
{
  std::string r;
  for (; i < s.args.size(); i++)
    r += (r.empty() ? "" : " ") + s.args[i];
  return r;
}

uint8_t mySlot()
{
  return g_session ? g_session->localSlot() : 0;
}

bool slotActive(uint8_t s)
{
  return s == mySlot() || g_session->player(s).active;
}

// --- dumps ---------------------------------------------------------------------------------------

// Invested skills (level > 0), "name:level", sorted: as stand-ins get them (PlayerInfo).
static std::string skillsJson(engine::Unit* u)
{
  std::vector<std::string> list;
  void* mgr = engine::field<void*>(u, engine::kCharSkillManager);
  void** skills = mgr ? engine::field<void**>(mgr, engine::kSkillManagerSkills) : nullptr;
  uint32_t count = mgr ? engine::field<uint32_t>(mgr, engine::kSkillManagerSkills + 4) : 0;
  for (uint32_t i = 0; skills && i < count && i < 128; i++)
  {
    void* sk = skills[i];
    int level = sk ? engine::field<int>(sk, 0xA4) : 0;
    auto* data = sk ? engine::field<uint8_t*>(sk, 0x5C) : nullptr;
    wchar_t name[96] = {};
    if (level > 0 && data && diag::readWString(reinterpret_cast<uintptr_t>(data) + 0xCC, name, 96))
      list.push_back(toUtf8(name) + ":" + std::to_string(level));
  }
  std::sort(list.begin(), list.end());
  std::string out = ",\"skillLevels\":[";
  for (size_t i = 0; i < list.size(); i++)
    out += (i ? ",\"" : "\"") + jsonEscape(list[i]) + "\"";
  return out + "]";
}

// Level and base stats, as stand-ins get them (PlayerState).
static std::string statsJson(engine::Unit* u)
{
  char b[128];
  std::snprintf(b, sizeof(b), ",\"stats\":[%d,%d,%d,%d,%d,%d]", engine::field<int>(u, engine::kUnitLevel),
                engine::field<int>(u, engine::kCharStrength), engine::field<int>(u, engine::kCharDexterity),
                engine::field<int>(u, engine::kCharMagic), engine::field<int>(u, engine::kCharDefense),
                engine::field<int>(u, engine::kCharBaseArmor));
  return b;
}

void dump(const std::string& tag)
{
  std::ostringstream o;
  engine::Level* level = currentLevel();
  o << "{\"tag\":\"" << jsonEscape(tag) << "\",\"time\":" << clock() << ",\"slot\":" << int(mySlot());
  char buf[64];
  std::snprintf(buf, sizeof(buf), "\"%08x\"", levelId(level));
  o << ",\"level\":" << buf << ",\"town\":" << (floor::isTownLevel(levelId(level)) ? "true" : "false");
  o << ",\"owner\":" << floor::currentOwner();
  if (level)
  {
    // CLevel +0x140 depth, +0x1F0 dungeon name (what quest spawners match against, 5F67E0).
    uint32_t depth = 0;
    wchar_t dungeon[64] = {};
    diag::readU32(reinterpret_cast<uintptr_t>(level) + 0x140, depth);
    diag::readWString(reinterpret_cast<uintptr_t>(level) + 0x1F0, dungeon, 64);
    o << ",\"depth\":" << static_cast<int32_t>(depth) << ",\"dungeon\":\"" << jsonEscape(toUtf8(dungeon)) << "\"";
  }
  unsigned groups = 0, waitingGroups = 0;
  spawns::countGroups(&groups, &waitingGroups);
  o << ",\"groups\":" << groups << ",\"groupsWaiting\":" << waitingGroups << ",\"waitingAt\":[";
  struct W
  {
    std::ostringstream* o;
    bool first;
  } wc{&o, true};
  spawns::listWaiting(
    [](float x, float z, void* p) {
      auto* c = static_cast<W*>(p);
      *c->o << (c->first ? "" : ",") << "[" << x << "," << z << "]";
      c->first = false;
    },
    &wc);
  o << "]";
  const floor::PartyFloor& pf = floor::partyFloor();
  std::snprintf(buf, sizeof(buf), "\"%08x\"", pf.levelId);
  o << ",\"party\":{\"known\":" << (pf.known ? "true" : "false") << ",\"level\":" << buf << ",\"owner\":" << pf.owner
    << "}";
  if (engine::Unit* m = me())
  {
    Vec3 p = unitPosition(m);
    o << ",\"me\":{\"x\":" << p.x << ",\"z\":" << p.z << ",\"health\":" << engine::field<float>(m, engine::kUnitHealth)
      << ",\"max\":" << engine::GetMaxHealth(m) << ",\"xp\":" << myXp() << ",\"dead\":" << (isDeadUnit(m) ? "true" : "false")
      << ",\"items\":" << backpackItems() << ",\"name\":\"" << jsonEscape(unitName(m)) << "\"" << statsJson(m) << skillsJson(m) << "}";
  }
  if (engine::Unit* m = me())
  {
    o << ",\"anims\":[";
    void* obj = engine::field<void*>(m, engine::kUnitAnimation);
    uintptr_t set = obj ? engine::field<uintptr_t>(obj, engine::kAnimationSet) : 0;
    uint32_t count = set ? engine::field<uint32_t>(reinterpret_cast<void*>(set), engine::kAnimSetCount) : 0;
    uintptr_t names = set ? engine::field<uintptr_t>(reinterpret_cast<void*>(set), engine::kAnimSetNames) : 0;
    for (uint32_t i = 0; names && i < count && i < 200; i++)
    {
      char n[64];
      if (diag::readString(names + i * engine::kAnimNameSize, n, sizeof(n)))
        o << (i ? "," : "") << "\"" << jsonEscape(n) << "\"";
    }
    o << "]";
  }
  o << ",\"avatars\":[";
  bool first = true;
  for (uint8_t s = 0; s < net::kMaxPlayers; s++)
  {
    engine::Unit* a = avatarOf(s);
    engine::Unit* pet = avatarPetOf(s);
    if (!a && !pet)
      continue;
    o << (first ? "" : ",") << "{\"slot\":" << int(s);
    first = false;
    if (a)
    {
      Vec3 p = unitPosition(a);
      o << ",\"x\":" << p.x << ",\"z\":" << p.z << ",\"health\":" << engine::field<float>(a, engine::kUnitHealth)
        << ",\"max\":" << engine::GetMaxHealth(a) << ",\"dead\":" << (isDeadUnit(a) ? "true" : "false") << ",\"name\":\""
        << jsonEscape(unitName(a)) << "\"" << statsJson(a) << skillsJson(a);
    }
    o << ",\"pet\":" << (pet ? "true" : "false") << "}";
  }
  o << "],\"monsters\":[";
  struct M
  {
    std::ostringstream* o;
    bool first;
  } mc{&o, true};
  monsters::forEachKnown(
    [](uint32_t id, engine::Unit* u, bool copy, bool dead, void* p) {
      auto* c = static_cast<M*>(p);
      Vec3 pos = unitPosition(u);
      char g[32];
      std::snprintf(g, sizeof(g), "%016llx", engine::field<uint64_t>(u, engine::kUnitGuid));
      *c->o << (c->first ? "" : ",") << "{\"id\":" << id << ",\"copy\":" << (copy ? "true" : "false") << ",\"guid\":\"" << g
            << "\",\"name\":\"" << jsonEscape(unitName(u)) << "\",\"health\":" << engine::field<float>(u, engine::kUnitHealth)
            << ",\"max\":" << engine::GetMaxHealth(u) << ",\"base\":" << engine::field<int>(u, engine::kUnitBaseHealth)
            << ",\"bonus\":" << engine::field<int>(u, engine::kUnitBonusHealth) << ",\"lvl\":" << engine::field<int>(u, engine::kUnitLevel)
            << ",\"x\":" << pos.x << ",\"z\":" << pos.z
            << ",\"dead\":" << (dead ? "true" : "false") << "}";
      c->first = false;
    },
    &mc);
  // Active quests (client+0x34 CQuestManager: CQuest* array at +0xC, count +0x10): raw fields while
  // the layout is being learned (see re/NOTES.md, quests).
  o << "],\"depths\":[";
  if (uintptr_t pl = localPlayer())
  {
    uint32_t data = 0, count = 0;
    diag::readU32(pl + 0x9EC, data);
    diag::readU32(pl + 0x9F0, count);
    for (uint32_t i = 0; data && i < count && i < 16; i++)
    {
      uint32_t e = 0, depth = 0;
      wchar_t name[64] = {};
      if (!diag::readU32(data + i * 4, e) || !e)
        continue;
      diag::readWString(e, name, 64);
      diag::readU32(e + 0x1C, depth);
      o << (i ? "," : "") << "{\"dungeon\":\"" << jsonEscape(toUtf8(name)) << "\",\"depth\":" << static_cast<int32_t>(depth) << "}";
    }
  }
  o << "],\"quests\":[";
  {
    uintptr_t qm = 0;
    uint32_t data = 0, count = 0;
    if (gameClient() && diag::readU32(gameClient() + 0x34, reinterpret_cast<uint32_t&>(qm)) && qm &&
        diag::readU32(qm + 0xC, data) && diag::readU32(qm + 0x10, count))
    {
      for (uint32_t i = 0; data && i < count && i < 64; i++)
      {
        uint32_t q = 0;
        if (!diag::readU32(data + i * 4, q) || !q)
          continue;
        o << (i ? "," : "") << "{\"ptr\":" << q;
        uint32_t v = 0;
        for (uint32_t off : {0x10u, 0x14u, 0x1ECu, 0xD0u, 0xE0u, 0xF0u, 0x1D4u})
        {
          diag::readU32(q + off, v);
          o << ",\"f" << std::hex << off << std::dec << "\":" << v;
        }
        {
          // Raw words while the layout is being learned (diffed across a quest kill).
          o << ",\"raw\":[";
          for (uint32_t off = 0; off < 0x200; off += 4)
          {
            uint32_t word = 0;
            diag::readU32(q + off, word);
            o << (off ? "," : "") << static_cast<int32_t>(word);
          }
          o << "]";
        }
        // Owner chain (0x5EB430 picks the target depth from [[+0x1C8]+8]'s depth in the quest's
        // dungeon) and the level spec found for it (+8).
        {
          uint32_t owner = 0, ownerPlayer = 0, spec = 0;
          diag::readU32(q + 0x1C8, owner);
          if (owner)
            diag::readU32(owner + 8, ownerPlayer);
          diag::readU32(q + 8, spec);
          o << ",\"owner\":" << owner << ",\"ownerPlayer\":" << ownerPlayer << ",\"me\":"
            << localPlayer() << ",\"spec\":" << spec << ",\"mgr\":" << qm;
        }
        // Spawners (5F67E0 per level, called from 5E8FC0): vectors at +0x154 / +0x164 / +0x174 of
        // vectors of spawner pointers. Raw fields while the layout is being learned.
        o << ",\"spawners\":[";
        bool firstSp = true;
        for (uint32_t list : {0x154u, 0x164u, 0x174u})
        {
          uint32_t outer = 0, outerCount = 0;
          if (!diag::readU32(q + list, outer) || !diag::readU32(q + list + 4, outerCount))
            continue;
          for (uint32_t a = 0; outer && a < outerCount && a < 16; a++)
          {
            uint32_t inner = 0, innerData = 0, innerCount = 0;
            if (!diag::readU32(outer + a * 4, inner) || !inner || !diag::readU32(inner, innerData) ||
                !diag::readU32(inner + 4, innerCount))
              continue;
            for (uint32_t b = 0; innerData && b < innerCount && b < 16; b++)
            {
              uint32_t sp = 0;
              if (!diag::readU32(innerData + b * 4, sp) || !sp)
                continue;
              o << (firstSp ? "" : ",") << "{\"list\":" << list << ",\"a\":" << a << ",\"b\":" << b;
              firstSp = false;
              for (uint32_t off : {0x10u, 0x20u, 0x28u, 0x2Cu, 0x10Cu, 0x110u, 0x120u})
              {
                uint32_t word = 0;
                diag::readU32(sp + off, word);
                o << ",\"f" << std::hex << off << std::dec << "\":" << static_cast<int32_t>(word);
              }
              for (uint32_t off : {0x40u, 0x43u, 0x124u, 0x125u})
              {
                uint32_t word = 0;
                diag::readU32(sp + off, word);
                o << ",\"b" << std::hex << off << std::dec << "\":" << (word & 0xFF);
              }
              o << "}";
            }
          }
        }
        o << "]";
        o << ",\"strings\":[";
        bool firstStr = true;
        for (uint32_t off = 0; off + 28 <= 0x300; off += 4)
        {
          wchar_t text[96];
          if (diag::readWString(q + off, text, 96) && text[0])
          {
            char b[16];
            std::snprintf(b, sizeof(b), "%x", off);
            o << (firstStr ? "" : ",") << "\"" << b << "=" << jsonEscape(toUtf8(text)) << "\"";
            firstStr = false;
          }
        }
        o << "]}";
      }
    }
  }
  o << "],\"skills\":[";
  if (engine::Unit* m = me())
  {
    // Diagnostics: the skill manager's array at +0x3C (count +0x40?), names and levels.
    void* mgr = engine::field<void*>(m, engine::kCharSkillManager);
    uint32_t data = 0, count = 0;
    if (mgr && diag::readU32(reinterpret_cast<uintptr_t>(mgr) + 0x3C, data) &&
        diag::readU32(reinterpret_cast<uintptr_t>(mgr) + 0x40, count))
    {
      for (uint32_t i = 0; data && i < count && i < 64; i++)
      {
        uint32_t sk = 0, lvlData = 0;
        wchar_t name[96] = {};
        if (!diag::readU32(data + i * 4, sk) || !sk)
          continue;
        diag::readU32(sk + 0x5C, lvlData);
        if (lvlData)
          diag::readWString(lvlData + 0xCC, name, 96);
        uint32_t lvl = 0;
        diag::readU32(sk + 0xA4, lvl);
        o << (i ? "," : "") << "{\"name\":\"" << jsonEscape(toUtf8(name)) << "\",\"level\":" << static_cast<int32_t>(lvl) << "}";
      }
    }
  }
  o << "],\"triggers\":[";
  {
    struct T
    {
      std::ostringstream* o;
      bool first;
    } tc{&o, true};
    triggers::forEachTrigger(
      [](void* t, void* p) {
        auto* c = static_cast<T*>(p);
        auto* u = static_cast<engine::Unit*>(t);
        Vec3 at = unitPosition(u);
        char g[32];
        std::snprintf(g, sizeof(g), "%016llx", engine::field<uint64_t>(u, engine::kUnitGuid));
        *c->o << (c->first ? "" : ",") << "{\"guid\":\"" << g << "\",\"name\":\"" << jsonEscape(unitName(u))
              << "\",\"x\":" << at.x << ",\"z\":" << at.z << "}";
        c->first = false;
      },
      &tc);
  }
  o << "],\"shared\":[";
  struct S
  {
    std::ostringstream* o;
    bool first;
  } sc{&o, true};
  loot::forEachShared(
    [](uint32_t key, void* unit, void* p) {
      auto* c = static_cast<S*>(p);
      *c->o << (c->first ? "" : ",") << "{\"key\":" << key << ",\"onground\":" << (unit ? "true" : "false") << "}";
      c->first = false;
    },
    &sc);
  o << "],\"notices\":[";
  for (size_t i = g_notices.size() > 12 ? g_notices.size() - 12 : 0; i < g_notices.size(); i++)
    o << (i == (g_notices.size() > 12 ? g_notices.size() - 12 : 0) ? "" : ",") << "\"" << jsonEscape(g_notices[i]) << "\"";
  o << "]}";
  std::ofstream f(g_outDir + L"\\dump_" + fromUtf8(tag) + L".json");
  f << o.str() << "\n";
}

// Quest manager (client+0x34): +0x1EC state of the listed quest named `name` (1 active, 2 done), or
// -1 when it is not in the list; `*player` gets the manager's player (+8).
static int questState(const std::string& name, uint32_t* player, bool* done = nullptr)
{
  uint32_t qm = 0, data = 0, count = 0;
  *player = 0;
  if (!gameClient() || !diag::readU32(gameClient() + 0x34, qm) || !qm || !diag::readU32(qm + 0xC, data) ||
      !diag::readU32(qm + 0x10, count))
    return -1;
  diag::readU32(qm + 8, *player);
  for (uint32_t i = 0; data && i < count && i < 64; i++)
  {
    uint32_t q = 0, state = 0;
    wchar_t text[96];
    if (diag::readU32(data + i * 4, q) && q && diag::readWString(q + 0x94, text, 96) &&
        _stricmp(toUtf8(text).c_str(), name.c_str()) == 0 && diag::readU32(q + 0x1EC, state))
    {
      uint32_t flags = 0;
      if (done && diag::readU32(q + 0x18, flags))
        *done = (flags & 0xFF) != 0; // +0x18: objectives met (set by the kill of its DEFEAT target)
      return static_cast<int>(state);
    }
  }
  return -1;
}

// --- commands ------------------------------------------------------------------------------------

Poll expectCheck(const Step& s, std::string& why)
{
  const std::string& what = s.args.size() > 1 ? s.args[1] : std::string();
  if (what == "avatar" || what == "noavatar" || what == "pet")
  {
    auto slot = static_cast<uint8_t>(intArg(s, 2));
    engine::Unit* u = what == "pet" ? avatarPetOf(slot) : avatarOf(slot);
    bool ok = what == "noavatar" ? !u : u != nullptr;
    why = u ? "present" : "absent";
    return ok ? Poll::Done : Poll::Wait;
  }
  if (what == "quest") // quest NAME [state]: listed (with that state), and the manager is ours
  {
    std::string name = s.args.size() > 2 ? s.args[2] : std::string();
    uint32_t player = 0;
    int state = questState(name, &player);
    bool mine = player && player == localPlayer();
    why = "state " + std::to_string(state) + (mine ? "" : ", quest manager on another unit");
    return mine && (s.args.size() > 3 ? state == intArg(s, 3) : state >= 0) ? Poll::Done : Poll::Wait;
  }
  if (what == "questdone") // questdone NAME: its objectives are met
  {
    std::string name = s.args.size() > 2 ? s.args[2] : std::string();
    uint32_t player = 0;
    bool done = false;
    int state = questState(name, &player, &done);
    why = "state " + std::to_string(state) + (done ? ", objectives met" : ", objectives open");
    return done ? Poll::Done : Poll::Wait;
  }
  if (what == "owner")
  {
    int want = intArg(s, 2);
    why = "owner " + std::to_string(floor::currentOwner());
    return floor::currentOwner() == want ? Poll::Done : Poll::Wait;
  }
  if (what == "monsters")
  {
    unsigned alive = 0;
    nearestMonster(1e9f, &alive);
    why = std::to_string(alive) + " alive";
    return static_cast<int>(alive) >= intArg(s, 2) ? Poll::Done : Poll::Wait;
  }
  if (what == "xpgain")
  {
    why = "xp " + std::to_string(g_xpMark) + " -> " + std::to_string(myXp());
    return myXp() > g_xpMark ? Poll::Done : Poll::Wait;
  }
  if (what == "notice")
  {
    std::string text = restFrom(s, 2);
    for (size_t i = g_noticeMark; i < g_notices.size(); i++)
      if (g_notices[i].find(text) != std::string::npos)
      {
        why = g_notices[i];
        return Poll::Done;
      }
    why = g_notices.empty() ? "no notices" : "last: " + g_notices.back();
    return Poll::Wait;
  }
  if (what == "items")
  {
    int now = backpackItems();
    why = std::to_string(g_itemsMark) + " -> " + std::to_string(now);
    return now - g_itemsMark >= intArg(s, 2) ? Poll::Done : Poll::Wait;
  }
  if (what == "shared")
  {
    int n = 0;
    loot::forEachShared([](uint32_t, void* unit, void* p) { *static_cast<int*>(p) += unit ? 1 : 0; }, &n);
    why = std::to_string(n) + " on the ground";
    return n >= intArg(s, 2) ? Poll::Done : Poll::Wait;
  }
  if (what == "anim")
  {
    auto slot = static_cast<uint8_t>(intArg(s, 2));
    std::string want = s.args.size() > 3 ? s.args[3] : std::string();
    std::string got = anim::lastReplayed(slot);
    why = got.empty() ? "nothing replayed" : "last " + got;
    return _stricmp(got.c_str(), want.c_str()) == 0 ? Poll::Done : Poll::Wait;
  }
  if (what == "area") // area waiting|fired|unfired: the trigger area of the last `teleport area`
  {
    std::string want = s.args.size() > 2 ? s.args[2] : std::string();
    bool waiting = false;
    int fired = spawns::areaState(g_area, &waiting);
    why = fired < 0 ? "no area" : "fired " + std::to_string(fired) + (waiting ? ", waiting for the party" : "");
    if (fired < 0)
      return Poll::Wait;
    if (want == "unfired") // must hold now, not eventually
      return fired == 0 ? Poll::Done : Poll::Fail;
    return (want == "fired" ? fired > 0 : waiting) ? Poll::Done : Poll::Wait;
  }
  if (what == "monsterscale" || what == "copyscale") // <x>: every monster run / copy shown here is at x
  {
    float lo = 0.0f, hi = 0.0f, want = floatArg(s, 2, 1.0f);
    if (!(what == "monsterscale" ? monsters::scaleRange(&lo, &hi) : monsters::copyScaleRange(&lo, &hi)))
    {
      why = "no monsters run here";
      return Poll::Wait;
    }
    char b[64];
    std::snprintf(b, sizeof(b), "multipliers %.2f..%.2f", lo, hi);
    why = b;
    return std::fabs(lo - want) < 0.01f && std::fabs(hi - want) < 0.01f ? Poll::Done : Poll::Wait;
  }
  if (what == "looks") // every copy here has its owner's look (names, champions, health)
  {
    unsigned n = monsters::copiesWithoutLook();
    why = std::to_string(n) + " copies still built from the type only";
    return n == 0 ? Poll::Done : Poll::Wait;
  }
  if (what == "minioncopies") // minioncopies <slot> <n>: copies of that player's minions shown here
  {
    unsigned n = minions::copiesShown(static_cast<uint8_t>(intArg(s, 2)));
    why = std::to_string(n) + " copies";
    return static_cast<int>(n) >= intArg(s, 3) ? Poll::Done : Poll::Wait;
  }
  if (what == "nominioncopies") // nominioncopies <slot>: no copy of that player's minions here
  {
    unsigned n = minions::copiesShown(static_cast<uint8_t>(intArg(s, 2)));
    why = std::to_string(n) + " copies";
    return n == 0 ? Poll::Done : Poll::Wait;
  }
  if (what == "minionhurt") // one of our minions has lost health (hits on its copies reach it)
  {
    float worst = 1.0f;
    for (uint8_t i = 0; i < 32; i++)
      if (engine::Unit* u = minions::localMinion(i))
        worst = std::min(worst, engine::field<float>(u, engine::kUnitHealth) / std::max(1, engine::GetMaxHealth(u)));
    char b[48];
    std::snprintf(b, sizeof(b), "lowest minion health %.0f%%", worst * 100.0f);
    why = b;
    return worst < 0.999f ? Poll::Done : Poll::Wait;
  }
  if (what == "minions") // minions <slot> <n>: that player's character here has n minions besides its pet
  {
    int slot = intArg(s, 2);
    engine::Unit* who = slot == mySlot() ? me() : avatarOf(static_cast<uint8_t>(slot));
    int n = 0;
    if (who)
    {
      auto** it = engine::field<engine::Unit**>(who, engine::kPlayerMinions);
      auto** end = engine::field<engine::Unit**>(who, engine::kPlayerMinions + 4);
      for (; it && it != end; ++it)
        if (*it && reinterpret_cast<uintptr_t>(*it) != localPet() && !isAvatarUnit(*it))
          n++;
    }
    why = std::to_string(n) + " minions";
    return n >= intArg(s, 3) ? Poll::Done : Poll::Wait;
  }
  if (what == "castsent" || what == "castplayed") // our casts sent / others' casts shown here, since mark
  {
    unsigned n = what == "castsent" ? skills::castsSent() - g_castsSentMark : skills::castsPlayed() - g_castsPlayedMark;
    why = std::to_string(n) + (what == "castsent" ? " casts sent" : " casts shown") + " since mark";
    return static_cast<int>(n) >= intArg(s, 2) ? Poll::Done : Poll::Wait;
  }
  if (what == "lootrolls") // lootrolls N: our game rolled treasure for N kills since mark (drops are random)
  {
    unsigned n = hooks::killTreasureRolls() - g_rollsMark;
    why = std::to_string(n) + " treasure rolls since mark";
    return static_cast<int>(n) >= intArg(s, 2) ? Poll::Done : Poll::Wait;
  }
  if (what == "groundloot")
  {
    int n = groundItemsNear(30.0f);
    why = std::to_string(g_groundMark) + " -> " + std::to_string(n) + " items on the ground nearby";
    return n > g_groundMark ? Poll::Done : Poll::Wait;
  }
  if (what == "moremonsters")
  {
    unsigned alive = 0;
    nearestMonster(1e9f, &alive);
    why = std::to_string(g_monstersMark) + " -> " + std::to_string(alive);
    return alive > g_monstersMark ? Poll::Done : Poll::Wait;
  }
  if (what == "noshared")
  {
    int n = 0;
    loot::forEachShared([](uint32_t, void* unit, void* p) { *static_cast<int*>(p) += unit ? 1 : 0; }, &n);
    why = std::to_string(n) + " on the ground";
    return n == 0 ? Poll::Done : Poll::Wait;
  }
  if (what == "newlevel")
  {
    char b[64];
    std::snprintf(b, sizeof(b), "marked %08x, now %08x", g_levelMark, levelId(currentLevel()));
    why = b;
    return currentLevel() && localPlayer() && levelId(currentLevel()) != g_levelMark ? Poll::Done : Poll::Wait;
  }
  if (what == "avatardead")
  {
    engine::Unit* a = avatarOf(static_cast<uint8_t>(intArg(s, 2)));
    why = !a ? "no stand-in" : isDeadUnit(a) ? "dead" : "alive";
    return a && isDeadUnit(a) ? Poll::Done : Poll::Wait;
  }
  if (what == "hurt")
  {
    float h = me() ? engine::field<float>(me(), engine::kUnitHealth) : 0.0f;
    char b[64];
    std::snprintf(b, sizeof(b), "health %.0f -> %.0f", g_healthMark, h);
    why = b;
    return h < g_healthMark - 0.5f ? Poll::Done : Poll::Wait;
  }
  if (what == "dead" || what == "alive")
  {
    bool dead = me() && isDeadUnit(me());
    why = dead ? "dead" : "alive";
    return (what == "dead") == dead ? Poll::Done : Poll::Wait;
  }
  if (what == "town" || what == "dungeon")
  {
    bool town = floor::isTownLevel(levelId(currentLevel()));
    why = town ? "in town" : "not in town";
    return (what == "town") == town && localPlayer() && currentLevel() ? Poll::Done : Poll::Wait;
  }
  if (what == "samelevel")
  {
    auto slot = static_cast<uint8_t>(intArg(s, 2));
    char b[64];
    std::snprintf(b, sizeof(b), "mine %08x, theirs %08x", levelId(currentLevel()), floor::slotLevel(slot));
    why = b;
    return floor::slotInGame(slot) && floor::slotLevel(slot) == levelId(currentLevel()) ? Poll::Done : Poll::Wait;
  }
  why = "unknown expectation";
  return Poll::Fail;
}

static bool g_shiftHeld = false;

Poll run(const Step& s, bool begin, std::string& why)
{
  const std::string& cmd = s.args[0];
  float elapsed = clock() - g_stepStart;
  engine::Level* level = currentLevel();

  if (cmd == "log")
  {
    TLMP_LOG("test: %s", restFrom(s, 1).c_str());
    return Poll::Done;
  }
  if (cmd == "wait")
    return elapsed >= floatArg(s, 1, 1.0f) ? Poll::Done : Poll::Wait;
  if (cmd == "players")
  {
    int n = 0;
    for (uint8_t i = 0; i < net::kMaxPlayers; i++)
      n += g_session->isConnected() && slotActive(i) ? 1 : 0;
    why = std::to_string(n) + " connected";
    return n >= intArg(s, 1, 2) ? Poll::Done : Poll::Wait;
  }
  if (cmd == "sync")
  {
    const std::string& name = s.args.size() > 1 ? s.args[1] : std::string("?");
    if (mySlot() >= net::kMaxPlayers)
    {
      why = "not connected"; // no slot (the session is gone): nobody to meet
      return Poll::Fail;
    }
    if (begin)
    {
      g_syncSeen[mySlot()].insert(name);
      net::Writer w(Msg::TestSync);
      w.u8(mySlot());
      w.str(name);
      if (g_session->isHost())
        g_session->sendToAll(w, true);
      else
        g_session->sendTo(0, w, true);
    }
    std::string missing;
    for (uint8_t i = 0; i < net::kMaxPlayers; i++)
      if (slotActive(i) && !g_syncSeen[i].count(name))
        missing += " " + std::to_string(i);
    why = missing.empty() ? "" : "waiting for slots" + missing;
    return missing.empty() ? Poll::Done : Poll::Wait;
  }
  if (cmd == "share" && s.args.size() > 1 && s.args[1] == "node")
  {
    // Test only: tell the others where our nearest unspawned monster group (at least <min> away) is;
    // groups are placed per machine, so only the floor owner's ones spawn.
    float x = 0.0f, z = 0.0f;
    if (!spawns::nearestWaitingGroup(floatArg(s, 2, 25.0f), &x, &z))
    {
      why = "no waiting monster group";
      return Poll::Fail;
    }
    char b[64];
    std::snprintf(b, sizeof(b), "at:%.2f,%.2f", x, z);
    net::Writer w(Msg::TestSync);
    w.u8(mySlot());
    w.str(b);
    if (g_session->isHost())
      g_session->sendToAll(w, true);
    else
      g_session->sendTo(0, w, true);
    why = b;
    return Poll::Done;
  }
  if (cmd == "clickslow")
  {
    // Press now, release on the next frame.
    static bool pressed = false;
    if (begin)
      pressed = false;
    if (pressed)
    {
      releaseMouse();
      pressed = false;
      return Poll::Done;
    }
    why = "not showing";
    pressed = clickWindow(s.args.size() > 1 ? s.args[1] : "", true);
    return Poll::Wait;
  }
  if (cmd == "settext" && s.args.size() > 2)
  {
    why = "not showing";
    return setWindowText(s.args[1], restFrom(s, 2)) ? Poll::Done : Poll::Wait;
  }
  if (cmd == "click")
  {
    if (clock() - g_lastAction < 0.5f)
      return Poll::Wait;
    g_lastAction = clock();
    why = "not showing";
    return clickWindow(s.args.size() > 1 ? s.args[1] : "") ? Poll::Done : Poll::Wait;
  }
  if (cmd == "ingame")
    return localPlayer() && level && party::enteredFromContinue() ? Poll::Done : Poll::Wait;
  if (cmd == "mark")
  {
    g_xpMark = myXp();
    g_healthMark = me() ? engine::field<float>(me(), engine::kUnitHealth) : 0.0f;
    g_itemsMark = backpackItems();
    g_levelMark = levelId(currentLevel());
    nearestMonster(1e9f, &g_monstersMark);
    g_groundMark = groundItemsNear(30.0f);
    g_rollsMark = hooks::killTreasureRolls();
    g_castsSentMark = skills::castsSent();
    g_castsPlayedMark = skills::castsPlayed();
    g_noticeMark = g_notices.size();
    return Poll::Done;
  }
  if (cmd == "dump")
  {
    dump(s.args.size() > 1 ? s.args[1] : "x");
    return Poll::Done;
  }
  if (cmd == "shot")
  {
    why = "no window";
    return screenshot(s.args.size() > 1 ? s.args[1] : "x") ? Poll::Done : Poll::Fail;
  }
  if (cmd == "expect")
    return expectCheck(s, why);
  if (cmd == "console")
  {
    // Runs a console command (cheats included: test mode only), e.g. "console QUESTACTIVE BardRandom".
    void* ui = gameClient() ? engine::field<void*>(reinterpret_cast<void*>(gameClient()), engine::kClientGameUI) : nullptr;
    void* console = ui ? engine::field<void*>(ui, engine::kGameUIConsole) : nullptr;
    if (!console)
    {
      why = "no console";
      return Poll::Wait;
    }
    std::wstring line = fromUtf8(restFrom(s, 1));
    reinterpret_cast<void(__thiscall*)(void*, vs::WString)>(engine::kConsoleExecute)(console, vs::make(line.c_str()));
    return Poll::Done;
  }
  if (cmd == "quit")
  {
    if (begin && g_window)
      PostMessageW(g_window, WM_CLOSE, 0, 0);
    return elapsed > 5.0f ? Poll::Done : Poll::Wait;
  }

  engine::Unit* m = me();
  if (!m || !level)
  {
    why = "not in the game";
    return Poll::Wait;
  }
  Vec3 myPos = unitPosition(m);

  if (cmd == "walkto")
  {
    const std::string& kind = s.args.size() > 1 ? s.args[1] : std::string();
    if (begin || clock() - g_lastAction > 1.0f)
    {
      g_lastAction = clock();
      if (kind == "player")
      {
        engine::Unit* a = avatarOf(static_cast<uint8_t>(intArg(s, 2)));
        if (!a)
        {
          why = "no such player here";
          return Poll::Wait;
        }
        g_target = unitPosition(a);
      }
      else if (kind == "far")
      {
        if (begin && !triggers::farthestTrigger(&g_target.x, &g_target.z))
        {
          why = "no doors or chests known here";
          return Poll::Fail;
        }
      }
      else if (kind == "node")
      {
        if (begin && !spawns::nearestWaitingGroup(floatArg(s, 2, 25.0f), &g_target.x, &g_target.z))
        {
          why = "no waiting monster group";
          return Poll::Fail;
        }
      }
      else if (kind == "monster")
      {
        engine::Unit* t = nearestMonster(1e9f);
        if (!t)
        {
          why = "no monsters";
          return Poll::Wait;
        }
        g_target = unitPosition(t);
      }
      else if (begin)
        g_target = Vec3{floatArg(s, 1), myPos.y, floatArg(s, 2)};
      engine::MoveTo(m, level, g_target.x, g_target.z);
    }
    float d = distXZ(myPos, g_target);
    char b[64];
    std::snprintf(b, sizeof(b), "%.1f away", d);
    why = b;
    return d <= (kind == "monster" ? 6.0f : kind == "node" || kind == "far" ? 8.0f : 2.5f) ? Poll::Done : Poll::Wait;
  }
  if (cmd == "teleport" && s.args.size() > 1 && s.args[1] == "player")
  {
    engine::Unit* a = avatarOf(static_cast<uint8_t>(intArg(s, 2)));
    if (!a)
    {
      why = "no such player here";
      return Poll::Wait;
    }
    Vec3 at = unitPosition(a);
    engine::setPosition(m, Vec3{at.x + 1.5f, at.y, at.z});
    return Poll::Done;
  }
  if (cmd == "teleport" && s.args.size() > 1 && s.args[1] == "monster")
  {
    // Test only: next to the monster closest to us among those at least <distance> from the host.
    engine::Unit* host = avatarOf(0);
    Vec3 hostPos = host ? unitPosition(host) : myPos;
    struct C
    {
      Vec3 me, host;
      float minFromHost, best;
      engine::Unit* unit;
    } c{myPos, hostPos, floatArg(s, 2, 50.0f), 1e9f, nullptr};
    monsters::forEachKnown(
      [](uint32_t, engine::Unit* u, bool, bool dead, void* p) {
        auto* c = static_cast<C*>(p);
        Vec3 at = unitPosition(u);
        if (dead || distXZ(at, c->host) < c->minFromHost)
          return;
        float d = distXZ(at, c->me);
        if (d < c->best)
        {
          c->best = d;
          c->unit = u;
        }
      },
      &c);
    if (!c.unit)
    {
      why = "no monster that far from the host";
      return Poll::Fail;
    }
    Vec3 at = unitPosition(c.unit);
    engine::setPosition(m, Vec3{at.x + 2.0f, at.y, at.z});
    char b[96];
    std::snprintf(b, sizeof(b), "next to %s, %.0f from the host", unitName(c.unit).c_str(), distXZ(at, hostPos));
    why = b;
    return Poll::Done;
  }
  if (cmd == "teleport" && s.args.size() > 1 && s.args[1] == "shared")
  {
    if (!g_hasShared)
    {
      why = "nothing shared yet";
      return Poll::Wait;
    }
    engine::setPosition(m, Vec3{g_sharedX, myPos.y, g_sharedZ});
    char b[64];
    std::snprintf(b, sizeof(b), "to (%.1f, %.1f)", g_sharedX, g_sharedZ);
    why = b;
    return Poll::Done;
  }
  if (cmd == "teleport" && s.args.size() > 1 && s.args[1] == "area")
  {
    // Test only: into the nearest trigger area that hasn't fired, at least <min> from here.
    Vec3 at{};
    if (!spawns::nearestArea(myPos, floatArg(s, 2, 0.0f), &at, &g_area))
    {
      why = "no trigger area seen";
      return Poll::Wait;
    }
    engine::setPosition(m, Vec3{at.x, myPos.y, at.z});
    char b[64];
    std::snprintf(b, sizeof(b), "to (%.1f, %.1f)", at.x, at.z);
    why = b;
    return Poll::Done;
  }
  if (cmd == "teleport")
  {
    // Test only: straight to the nearest monster group still waiting (no walkable path needed).
    float x = 0.0f, z = 0.0f;
    if (!spawns::nearestWaitingGroup(floatArg(s, 2, 25.0f), &x, &z))
    {
      why = "no waiting monster group";
      return Poll::Fail;
    }
    engine::setPosition(m, Vec3{x, myPos.y, z});
    char b[64];
    std::snprintf(b, sizeof(b), "to (%.1f, %.1f)", x, z);
    why = b;
    return Poll::Done;
  }
  if (cmd == "key" && s.args.size() > 2 && s.args[1] == "shift")
  {
    // Test only: a real Shift press (the whole machine sees it; released again by `key shift up` or
    // when the script ends).
    bool down = s.args[2] == "down";
    keybd_event(VK_SHIFT, 0, down ? 0 : KEYEVENTF_KEYUP, 0);
    g_shiftHeld = down;
    return Poll::Done;
  }
  if (cmd == "killquest")
  {
    // Test only: next to a living monster some quest placed (tagged, engine.h "quests") and kill it.
    if (begin)
      g_targetUnit = nullptr;
    if (!g_targetUnit)
    {
      monsters::forEachKnown(
        [](uint32_t, engine::Unit* u, bool, bool dead, void* p) {
          if (!dead && engine::field<uint64_t>(u, engine::kUnitQuestGuid) != ~0ull)
            *static_cast<engine::Unit**>(p) = u;
        },
        &g_targetUnit);
      if (!g_targetUnit)
      {
        why = "no quest monster here";
        return Poll::Wait; // retried until the step's timeout
      }
      Vec3 at = unitPosition(g_targetUnit);
      engine::setPosition(m, Vec3{at.x + 1.5f, at.y, at.z});
      if (s.args.size() > 1 && s.args[1] == "approach")
        return Poll::Done; // killquest approach: just go there
    }
    if (knownDead(g_targetUnit))
      return Poll::Done;
    if (clock() - g_lastAction < 0.3f)
      return Poll::Wait;
    g_lastAction = clock();
    Vec3 src = unitPosition(m), dir{0.0f, 0.0f, 0.0f};
    engine::TakeDamage(g_targetUnit, level, &src, 5000.0f, &dir, 0.0f, m, 1, 1, 0, 0, 0);
    why = "target " + unitName(g_targetUnit);
    return Poll::Wait;
  }
  if (cmd == "hit" || cmd == "killnear")
  {
    if (begin)
      g_targetUnit = nearestMonster(kReach);
    if (!g_targetUnit)
    {
      why = "no monster within reach";
      return Poll::Fail;
    }
    if (cmd == "killnear" && knownDead(g_targetUnit))
      return Poll::Done;
    if (!begin && clock() - g_lastAction < 0.3f)
      return Poll::Wait;
    g_lastAction = clock();
    Vec3 src = myPos, dir{0.0f, 0.0f, 0.0f};
    float dmg = cmd == "hit" ? floatArg(s, 1, 10.0f) : 5000.0f;
    engine::TakeDamage(g_targetUnit, level, &src, dmg, &dir, 0.0f, m, 1, 1, 0, 0, 0);
    why = "target " + unitName(g_targetUnit);
    return cmd == "hit" ? Poll::Done : Poll::Wait;
  }
  if (cmd == "anim")
  {
    why = "no such animation";
    return anim::playOneShot(m, s.args.size() > 1 ? s.args[1].c_str() : "") ? Poll::Done : Poll::Fail;
  }
  if (cmd == "skill" && s.args.size() > 1)
  {
    // Test only: use a skill the way player input does (0x414790): aim at the nearest monster
    // (or straight ahead), then UseSkill. skill right | skill slot <0-9>
    uint64_t guid = s.args[1] == "right" ? engine::field<uint64_t>(m, engine::kCharRightSkill)
                                         : engine::field<uint64_t>(m, engine::kCharHotkeySkills + 8 * intArg(s, 2));
    if (guid == ~0ull || !guid)
    {
      why = "no skill in that slot";
      return Poll::Fail;
    }
    engine::Unit* target = nearestMonster(25.0f);
    Vec3 at = target ? unitPosition(target) : Vec3{myPos.x + 5.0f, myPos.y, myPos.z};
    engine::AimAt(m, at.x, at.z);
    engine::SetTargetUnit(m, target);
    uint32_t r = 0;
    if (s.args.back() == "input")
    {
      // Through CGameClient's own use-skill (0x414790), as a click on the skill does.
      using ClientUseSkillFn = uint8_t(__thiscall*)(void* client, uint32_t lo, uint32_t hi, char a, char b);
      r = reinterpret_cast<ClientUseSkillFn>(0x00414790)(reinterpret_cast<void*>(gameClient()),
                                                          static_cast<uint32_t>(guid), static_cast<uint32_t>(guid >> 32), 0, 0);
    }
    else
      r = reinterpret_cast<engine::UseSkillFn>(engine::kUseSkill)(m, static_cast<uint32_t>(guid),
                                                                  static_cast<uint32_t>(guid >> 32));
    char b[96];
    std::snprintf(b, sizeof(b), "skill %016llx at (%.1f, %.1f) -> %u", guid, at.x, at.z, r);
    why = b;
    return Poll::Done; // 0 may only mean "queued until in range": the game casts it when it gets there
  }
  if (cmd == "useportal")
  {
    // Test only: step into the nearest town portal, as a click on it does (Interact).
    struct C
    {
      Vec3 from;
      float best;
      engine::Unit* unit;
    } c{myPos, 1e9f, nullptr};
    triggers::forEachTrigger(
      [](void* t, void* p) {
        auto* c = static_cast<C*>(p);
        auto* u = static_cast<engine::Unit*>(t);
        float d = distXZ(unitPosition(u), c->from);
        if (engine::field<uint64_t>(u, engine::kUnitGuid) == engine::kTownPortalGuid && d < c->best)
        {
          c->best = d;
          c->unit = u;
        }
      },
      &c);
    if (!c.unit)
    {
      why = "no town portal";
      return Poll::Wait;
    }
    Vec3 at = unitPosition(c.unit);
    engine::setPosition(m, Vec3{at.x + 1.0f, myPos.y, at.z});
    reinterpret_cast<engine::TriggerInteractFn>(engine::kTriggerInteract)(c.unit, m);
    return Poll::Done;
  }
  if (cmd == "killminion")
  {
    // Test only: our first minion dies.
    engine::Unit* u = minions::localMinion(0);
    if (!u)
    {
      why = "no minion";
      return Poll::Wait;
    }
    engine::kill(u);
    return Poll::Done;
  }
  if (cmd == "hitminioncopy")
  {
    // Test only: a monster's hit on our copy of that player's minion, through TakeDamage as the
    // game would deal it (the copy forwards it to the real minion).
    engine::Unit* copy = minions::anyCopy(static_cast<uint8_t>(intArg(s, 1)));
    engine::Unit* monster = nearestMonster(1e9f);
    if (!copy || !monster)
    {
      why = !copy ? "no minion copy" : "no monster";
      return Poll::Wait;
    }
    Vec3 src = unitPosition(monster), dir{0.0f, 0.0f, 0.0f};
    engine::TakeDamage(copy, level, &src, floatArg(s, 2, 5.0f), &dir, 0.0f, monster, 1, 1, 0, 0, 0);
    return Poll::Done;
  }
  if (cmd == "skilllevel" && s.args.size() > 2)
  {
    // Test only: skilllevel <level> <name...>: invest in a skill of ours as a skill point does.
    std::string name = restFrom(s, 2);
    vs::WString w = vs::make(fromUtf8(name).c_str());
    void* mgr = engine::field<void*>(m, engine::kCharSkillManager);
    void* skill = mgr ? engine::FindSkillByName(mgr, &w) : nullptr;
    vs::destroy(&w);
    why = name;
    if (!skill)
      return Poll::Fail;
    engine::SetSkillLevel(mgr, skill, intArg(s, 1));
    return Poll::Done;
  }
  if (cmd == "learnskill" && s.args.size() > 1)
  {
    // Test only: learn a skill by name (e.g. learnskill Summon Imp) and put it on the right button.
    std::string name = restFrom(s, 1);
    why = name;
    return skills::learnForTest(m, name) ? Poll::Done : Poll::Fail;
  }
  if (cmd == "hardcore" && s.args.size() > 1)
  {
    // Test only (test profiles): make this character hardcore in memory, so a death is final.
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(m) + engine::kPlayerHardcore) = s.args[1] == "on" ? 1 : 0;
    return Poll::Done;
  }
  if (cmd == "die")
  {
    engine::kill(m);
    return Poll::Done;
  }
  if (cmd == "drop")
  {
    void* item = backpackEquipment();
    void* ui = engine::field<void*>(reinterpret_cast<void*>(gameClient()), engine::kClientGameUI);
    if (!item || !ui)
    {
      why = item ? "no game UI" : "no equipment in the backpack";
      return Poll::Fail;
    }
    // As the UI does with the item on the cursor: out of the inventory, onto the ground.
    void* inv = engine::field<void*>(m, engine::kUnitInventory);
    engine::RemoveItem(inv, item);
    auto* held = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ui) + engine::kGameUIHeldItem);
    void* before = *held;
    *held = item;
    Vec3 at{myPos.x + 1.5f, myPos.y, myPos.z};
    engine::AddItemToLevel(level, item, &at, 1);
    *held = before;
    why = "dropped " + unitName(item);
    return Poll::Done;
  }
  if (cmd == "pickup")
  {
    if (begin)
    {
      struct P
      {
        Vec3 from;
        float best;
        void* item;
        uint32_t key;
      } p{myPos, 25.0f, nullptr, 0};
      loot::forEachShared(
        [](uint32_t key, void* unit, void* ctx) {
          auto* c = static_cast<P*>(ctx);
          if (!unit)
            return;
          float d = distXZ(unitPosition(unit), c->from);
          if (d < c->best)
          {
            c->best = d;
            c->item = unit;
            c->key = key;
          }
        },
        &p);
      if (!p.item)
      {
        why = "no shared item near";
        return Poll::Fail;
      }
      g_sharedKey = p.key;
      g_itemsMark = backpackItems();
      engine::PickUp(m, p.item, level);
    }
    bool stillShared = false;
    struct K
    {
      uint32_t key;
      bool* found;
    } k{g_sharedKey, &stillShared};
    loot::forEachShared(
      [](uint32_t key, void*, void* ctx) {
        auto* c = static_cast<K*>(ctx);
        if (key == c->key)
          *c->found = true;
      },
      &k);
    why = stillShared ? "still on the ground" : "items " + std::to_string(backpackItems());
    return !stillShared && backpackItems() > g_itemsMark ? Poll::Done : Poll::Wait;
  }
  if (cmd == "townportal")
  {
    changeLevel(L"Town", -99);
    return Poll::Done;
  }
  if (cmd == "publicip" && s.args.size() > 1)
  {
    net::upnp::setPublicAddressForTest(s.args[1]); // a placeholder for the status line
    return Poll::Done;
  }
  if (cmd == "lookuppublicip")
  {
    // The real lookup (one HTTPS request to api.ipify.org); passes once it has an answer.
    net::upnp::lookupPublicAddress();
    std::string ip = net::upnp::publicAddress();
    why = ip.empty() ? "no answer yet" : "found (" + std::to_string(ip.size()) + " characters)";
    return ip.empty() ? Poll::Wait : Poll::Done;
  }
  if (cmd == "upnpprobe")
  {
    // Read-only: does a UPnP router answer? (No port is mapped.)
    std::string detail;
    bool ok = net::upnp::probe(&detail);
    why = detail;
    TLMP_LOG("test: upnp probe: %s", detail.c_str());
    return ok ? Poll::Done : Poll::Fail;
  }
  if (cmd == "waypoint" && s.args.size() > 2)
  {
    // Test only: as the waypoint menu does (0x5B6481): ChangeLevel(dungeon, 0, floor, waypoint 1, "", 0).
    void* client = reinterpret_cast<void*>(gameClient());
    if (!client)
      return Poll::Fail;
    std::wstring dungeon = fromUtf8(s.args[1]);
    reinterpret_cast<engine::ChangeLevelFn>(engine::kChangeLevel)(client, vs::make(dungeon.c_str()), 0, intArg(s, 2), 1,
                                                                  vs::make(L""), 0);
    return Poll::Done;
  }
  if (cmd == "stairs")
  {
    changeLevel(L"", s.args.size() > 1 && s.args[1] == "up" ? -1 : 1);
    return Poll::Done;
  }
  why = "unknown command";
  return Poll::Fail;
}

void finish()
{
  g_active = false;
  if (g_shiftHeld)
  {
    keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    g_shiftHeld = false;
  }
  char line[128];
  std::snprintf(line, sizeof(line), "DONE: %u passed, %u failed", g_passed, g_failed);
  TLMP_LOG("test: %s", line);
  std::ofstream f(g_outDir + L"\\results.txt", std::ios::app);
  f << line << "\n";
}

} // namespace

void start(net::Session* session, const std::wstring& scriptPath)
{
  g_session = session;
  if (scriptPath.empty())
    return;
  std::ifstream in(scriptPath);
  if (!in)
  {
    TLMP_LOG("test: cannot open scenario '%ls'", scriptPath.c_str());
    return;
  }
  int lineNo = 0;
  std::string line;
  while (std::getline(in, line))
  {
    lineNo++;
    if (auto hash = line.find('#'); hash != std::string::npos)
      line.resize(hash);
    std::istringstream words(line);
    Step st;
    st.line = lineNo;
    std::string w;
    while (words >> w)
    {
      if (w.rfind("timeout=", 0) == 0)
        st.timeout = static_cast<float>(std::atof(w.c_str() + 8));
      else
        st.args.push_back(w);
    }
    if (st.args.empty())
      continue;
    if (st.args[0] == "expect" && st.timeout == kDefaultTimeout)
      st.timeout = kExpectTimeout;
    g_steps.push_back(std::move(st));
  }

  // Output next to the game: tlmp_test\<profile>\ (the profile tells host and clients apart).
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::wstring profile = L"default";
  for (int i = 1; argv && i + 1 < argc; i++)
    if (_wcsicmp(argv[i], L"-tlmp-profile") == 0)
      profile = argv[i + 1];
  LocalFree(argv);
  g_profile = toUtf8(profile.c_str());
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  dir.resize(dir.find_last_of(L"\\/"));
  CreateDirectoryW((dir + L"\\tlmp_test").c_str(), nullptr);
  g_outDir = dir + L"\\tlmp_test\\" + profile;
  CreateDirectoryW(g_outDir.c_str(), nullptr);
  DeleteFileW((g_outDir + L"\\results.txt").c_str());
  g_active = !g_steps.empty();
  TLMP_LOG("test: scenario '%ls' with %u steps, results in %ls", scriptPath.c_str(), static_cast<unsigned>(g_steps.size()),
           g_outDir.c_str());
}

bool active()
{
  return g_active;
}

void onNotice(const std::string& text)
{
  if (g_active)
    g_notices.push_back(text);
}

void onMessage(uint8_t fromSlot, Msg type, net::Reader& r)
{
  if (type != Msg::TestSync)
    return;
  uint8_t slot = r.u8();
  std::string name = r.str();
  if (!r.ok() || slot >= net::kMaxPlayers)
    return;
  if (name.compare(0, 3, "at:") == 0 && std::sscanf(name.c_str() + 3, "%f,%f", &g_sharedX, &g_sharedZ) == 2)
    g_hasShared = true; // `share node` from another machine
  g_syncSeen[slot].insert(name);
  if (g_session && g_session->isHost())
  {
    net::Writer w(Msg::TestSync);
    w.u8(slot);
    w.str(name);
    for (uint8_t s = 1; s < net::kMaxPlayers; s++)
      if (s != fromSlot && g_session->player(s).active)
        g_session->sendTo(s, w, true);
  }
}

void tick()
{
  if (!g_active)
    return;
  placeWindow();
  if (g_pc >= g_steps.size())
  {
    finish();
    return;
  }
  const Step& s = g_steps[g_pc];
  bool begin = !g_begun;
  if (begin)
  {
    g_begun = true;
    g_stepStart = clock();
    g_lastAction = -100.0f;
    TLMP_LOG("test: line %d: %s", s.line, restFrom(s, 0).c_str());
  }
  std::string why;
  Poll p = run(s, begin, why);
  if (p == Poll::Wait && clock() - g_stepStart > s.timeout)
  {
    p = Poll::Fail;
    why = "timed out" + (why.empty() ? std::string() : " (" + why + ")");
  }
  if (p == Poll::Wait)
    return;
  bool check = s.args[0] == "expect" || s.args[0] == "pickup" || s.args[0] == "killnear" || s.args[0] == "teleport" ||
               s.args[0] == "skill" || s.args[0] == "learnskill" || s.args[0] == "skilllevel" || s.args[0] == "upnpprobe" ||
               s.args[0] == "lookuppublicip";
  if (p == Poll::Fail)
  {
    g_failed++;
    result("FAIL", s, why);
  }
  else if (check)
  {
    g_passed++;
    result("PASS", s, why);
  }
  g_pc++;
  g_begun = false;
}

} // namespace tlmp::game::autotest
