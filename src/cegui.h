#pragma once

#include <string>

// The game's GUI library, CEGUI 0.6 (CEGUIBase.dll exports, MSVC2008 ABI), for our own windows:
// the title screen's co-op buttons (menu.h) and the chat box (chat.h). Built from the game's own
// widget types (GuiLook/StandardButton, GuiLook/Editbox, GuiLook/StaticText ...). A Window derives
// from PropertySet first, so a Window* is also its PropertySet*. Main thread only.
namespace tlmp::cegui {

struct Api
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
  // Optional (checked where used):
  void(__thiscall* deactivate)(void* window) = nullptr;
  void*(__thiscall* activeChild)(void* window) = nullptr; // the deepest active window under it
  const void*(__thiscall* type)(const void* window) = nullptr;
  bool(__thiscall* isActive)(const void* window) = nullptr;
  void(__thiscall* setAlpha)(void* window, float alpha) = nullptr;
  void(__thiscall* moveToFront)(void* window) = nullptr;
  void*(__thiscall* font)(const void* window, bool useDefault) = nullptr;
  float(__thiscall* textExtent)(void* font, const void* text, float xScale) = nullptr;
  float(__thiscall* lineSpacing)(const void* font, float yScale) = nullptr;
  void(__thiscall* setCarat)(void* editbox, unsigned index) = nullptr;
  unsigned(__thiscall* childCount)(const void* window) = nullptr;
  void*(__thiscall* childAt)(const void* window, unsigned index) = nullptr;
  bool ok = false; // the required ones were all found
};

extern Api api;

// Looks up the exports (once CEGUIBase.dll is loaded); false if any required one is missing.
bool resolve();

// A CEGUI::String for the duration of a call.
struct Str
{
  alignas(8) unsigned char raw[256] = {};
  explicit Str(const std::string& s);
  ~Str();
  Str(const Str&) = delete;
  Str& operator=(const Str&) = delete;
  const void* p() const { return raw; }
};

void* sheet();                        // the root window shown now (nullptr if none)
void* windowNamed(const char* name);  // nullptr if there is none
// CEGUI throws for a property the window type doesn't have: logged and ignored.
void prop(void* w, const char* name, const std::string& value);
void text(void* w, const std::string& value);
std::string textOf(const void* w);
std::string typeOf(const void* w);
// The first window under `w` (itself included) whose type contains `type`.
void* findByType(void* w, const char* type);
// Creates a window of `type` under `parent` (UnifiedPosition / UnifiedSize strings).
void* make(void* parent, const char* type, const char* name, const char* pos, const char* size,
           const std::string& label);
// A click on a button: it was pushed and is now released (wasPushed carries the state between
// frames). Not checking that the mouse is still on it: the game feeds CEGUI the real cursor, which
// a click injected while the window is in the background doesn't move.
bool clicked(void* button, bool& wasPushed);

} // namespace tlmp::cegui
