#include "cegui.h"
#include "log.h"

#include <windows.h>

namespace tlmp::cegui {

Api api;

bool resolve()
{
  if (api.ok)
    return true;
  HMODULE m = GetModuleHandleW(L"CEGUIBase.dll");
  if (!m)
    return false;
  auto get = [&](const char* n) { return GetProcAddress(m, n); };
  api.system = reinterpret_cast<void* (*)()>(get("?getSingleton@System@CEGUI@@SAAAV12@XZ"));
  api.sheet = reinterpret_cast<void*(__thiscall*)(void*)>(get("?getGUISheet@System@CEGUI@@QBEPAVWindow@2@XZ"));
  api.search = reinterpret_cast<void*(__thiscall*)(const void*, const void*)>(
    get("?recursiveChildSearch@Window@CEGUI@@QBEPAV12@ABVString@2@@Z"));
  api.strCtor = reinterpret_cast<void*(__thiscall*)(void*, const unsigned char*)>(get("??0String@CEGUI@@QAE@PBE@Z"));
  api.strDtor = reinterpret_cast<void(__thiscall*)(void*)>(get("??1String@CEGUI@@QAE@XZ"));
  api.cStr = reinterpret_cast<const char*(__thiscall*)(const void*)>(get("?c_str@String@CEGUI@@QBEPBDXZ"));
  api.windowManager =
    reinterpret_cast<void**>(get("?ms_Singleton@?$Singleton@VWindowManager@CEGUI@@@CEGUI@@1PAVWindowManager@2@A"));
  api.create = reinterpret_cast<void*(__thiscall*)(void*, const void*, const void*, const void*)>(
    get("?createWindow@WindowManager@CEGUI@@QAEPAVWindow@2@ABVString@2@00@Z"));
  api.present = reinterpret_cast<bool(__thiscall*)(const void*, const void*)>(
    get("?isWindowPresent@WindowManager@CEGUI@@QBE_NABVString@2@@Z"));
  api.getWindow = reinterpret_cast<void*(__thiscall*)(const void*, const void*)>(
    get("?getWindow@WindowManager@CEGUI@@QBEPAVWindow@2@ABVString@2@@Z"));
  api.destroy = reinterpret_cast<void(__thiscall*)(void*, void*)>(get("?destroyWindow@WindowManager@CEGUI@@QAEXPAVWindow@2@@Z"));
  api.setProperty = reinterpret_cast<void(__thiscall*)(void*, const void*, const void*)>(
    get("?setProperty@PropertySet@CEGUI@@QAEXABVString@2@0@Z"));
  api.addChild = reinterpret_cast<void(__thiscall*)(void*, void*)>(get("?addChildWindow@Window@CEGUI@@QAEXPAV12@@Z"));
  api.setText = reinterpret_cast<void(__thiscall*)(void*, const void*)>(get("?setText@Window@CEGUI@@QAEXABVString@2@@Z"));
  api.getText = reinterpret_cast<const void*(__thiscall*)(const void*)>(get("?getText@Window@CEGUI@@QBEABVString@2@XZ"));
  api.setVisible = reinterpret_cast<void(__thiscall*)(void*, bool)>(get("?setVisible@Window@CEGUI@@QAEX_N@Z"));
  api.visible = reinterpret_cast<bool(__thiscall*)(const void*, bool)>(get("?isVisible@Window@CEGUI@@QBE_N_N@Z"));
  api.pushed = reinterpret_cast<bool(__thiscall*)(const void*)>(get("?isPushed@ButtonBase@CEGUI@@QBE_NXZ"));
  api.hovering = reinterpret_cast<bool(__thiscall*)(const void*)>(get("?isHovering@ButtonBase@CEGUI@@QBE_NXZ"));
  api.parent = reinterpret_cast<void*(__thiscall*)(const void*)>(get("?getParent@Window@CEGUI@@QBEPAV12@XZ"));
  api.activate = reinterpret_cast<void(__thiscall*)(void*)>(get("?activate@Window@CEGUI@@QAEXXZ"));
  api.deactivate = reinterpret_cast<void(__thiscall*)(void*)>(get("?deactivate@Window@CEGUI@@QAEXXZ"));
  api.activeChild = reinterpret_cast<void*(__thiscall*)(void*)>(get("?getActiveChild@Window@CEGUI@@QAEPAV12@XZ"));
  api.type = reinterpret_cast<const void*(__thiscall*)(const void*)>(get("?getType@Window@CEGUI@@QBEABVString@2@XZ"));
  api.isActive = reinterpret_cast<bool(__thiscall*)(const void*)>(get("?isActive@Window@CEGUI@@QBE_NXZ"));
  api.setAlpha = reinterpret_cast<void(__thiscall*)(void*, float)>(get("?setAlpha@Window@CEGUI@@QAEXM@Z"));
  api.moveToFront = reinterpret_cast<void(__thiscall*)(void*)>(get("?moveToFront@Window@CEGUI@@QAEXXZ"));
  api.font = reinterpret_cast<void*(__thiscall*)(const void*, bool)>(get("?getFont@Window@CEGUI@@QBEPAVFont@2@_N@Z"));
  api.textExtent =
    reinterpret_cast<float(__thiscall*)(void*, const void*, float)>(get("?getTextExtent@Font@CEGUI@@QAEMABVString@2@M@Z"));
  api.lineSpacing = reinterpret_cast<float(__thiscall*)(const void*, float)>(get("?getLineSpacing@Font@CEGUI@@QBEMM@Z"));
  api.setCarat = reinterpret_cast<void(__thiscall*)(void*, unsigned)>(get("?setCaratIndex@Editbox@CEGUI@@QAEXI@Z"));
  api.childCount = reinterpret_cast<unsigned(__thiscall*)(const void*)>(get("?getChildCount@Window@CEGUI@@QBEIXZ"));
  api.childAt = reinterpret_cast<void*(__thiscall*)(const void*, unsigned)>(get("?getChildAtIdx@Window@CEGUI@@QBEPAV12@I@Z"));
  api.ok = api.system && api.sheet && api.search && api.strCtor && api.strDtor && api.cStr && api.windowManager &&
           api.create && api.present && api.getWindow && api.setProperty && api.addChild && api.setText &&
           api.getText && api.setVisible && api.visible && api.pushed && api.hovering && api.parent;
  if (!api.ok)
    TLMP_LOG("cegui: GUI functions not found; no co-op buttons or chat box");
  return api.ok;
}

Str::Str(const std::string& s)
{
  api.strCtor(raw, reinterpret_cast<const unsigned char*>(s.c_str()));
}

Str::~Str()
{
  api.strDtor(raw);
}

void* sheet()
{
  void* system = api.system ? api.system() : nullptr;
  return system ? api.sheet(system) : nullptr;
}

void* windowNamed(const char* name)
{
  void* wm = *api.windowManager;
  Str n(name);
  return wm && api.present(wm, n.p()) ? api.getWindow(wm, n.p()) : nullptr;
}

void prop(void* w, const char* name, const std::string& value)
{
  Str n(name), v(value);
  try
  {
    api.setProperty(w, n.p(), v.p());
  }
  catch (...)
  {
    TLMP_LOG("cegui: CEGUI refused property %s = %s", name, value.c_str());
  }
}

void text(void* w, const std::string& value)
{
  Str v(value);
  api.setText(w, v.p());
}

std::string textOf(const void* w)
{
  const void* s = w ? api.getText(w) : nullptr;
  const char* c = s ? api.cStr(s) : nullptr;
  return c ? c : "";
}

std::string typeOf(const void* w)
{
  const void* s = w && api.type ? api.type(w) : nullptr;
  const char* c = s ? api.cStr(s) : nullptr;
  return c ? c : "";
}

void* findByType(void* w, const char* type)
{
  if (!w)
    return nullptr;
  if (typeOf(w).find(type) != std::string::npos)
    return w;
  unsigned n = api.childCount && api.childAt ? api.childCount(w) : 0;
  for (unsigned i = 0; i < n; i++)
    if (void* found = findByType(api.childAt(w, i), type))
      return found;
  return nullptr;
}

void* make(void* parent, const char* type, const char* name, const char* pos, const char* size,
           const std::string& label)
{
  void* wm = *api.windowManager;
  Str t(type), n(name), prefix("");
  void* w = nullptr;
  try
  {
    w = api.create(wm, t.p(), n.p(), prefix.p());
  }
  catch (...)
  {
    TLMP_LOG("cegui: could not create %s (%s)", name, type);
    return nullptr;
  }
  if (!w)
    return nullptr;
  prop(w, "UnifiedPosition", pos);
  prop(w, "UnifiedSize", size);
  text(w, label);
  api.addChild(parent, w);
  return w;
}

bool clicked(void* button, bool& wasPushed)
{
  if (!button || !api.visible(button, false))
  {
    wasPushed = false;
    return false;
  }
  bool now = api.pushed(button);
  bool click = wasPushed && !now;
  wasPushed = now;
  return click;
}

} // namespace tlmp::cegui
