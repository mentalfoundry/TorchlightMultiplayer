#include "hud.h"
#include "cegui.h"
#include "log.h"

#include <cmath>
#include <cstdio>

namespace tlmp::hud {

using namespace cegui;

namespace {

constexpr int kFrames = 3;      // the other players
constexpr int kTop = 160;       // px: under the pet's portrait (pethud.layout, 154 px tall)
constexpr int kSpacing = 40;    // px between frames
constexpr int kBarWidth = 110;  // px
constexpr const char* kRoot = "TLMP_Party";

struct Shown
{
  bool visible = false;
  std::string label, note;
  int percent = -1;
  bool dead = false, away = false;
};
Shown g_shown[kFrames];
std::string g_text; // for tests

std::string name(const char* what, int k)
{
  return std::string("TLMP_Party") + what + std::to_string(k);
}

void build(void* sheet)
{
  void* root = make(sheet, "DefaultWindow", kRoot, "{{0,0},{0,0}}", "{{1,0},{1,0}}", "");
  if (!root)
    return;
  prop(root, "MousePassThroughEnabled", "True");
  for (int k = 0; k < kFrames; k++)
  {
    char pos[64];
    std::snprintf(pos, sizeof(pos), "{{0,8},{0,%d}}", kTop + k * kSpacing);
    void* frame = make(root, "DefaultWindow", name("Frame", k).c_str(), pos, "{{0,260},{0,38}}", "");
    if (!frame)
      return;
    prop(frame, "MousePassThroughEnabled", "True");
    api.setVisible(frame, false);
    if (void* label = make(frame, "GuiLook/StaticTextOutline", name("Name", k).c_str(), "{{0,2},{0,0}}",
                           "{{0,250},{0,20}}", ""))
    {
      prop(label, "Font", "Serif");
      prop(label, "MousePassThroughEnabled", "True");
    }
    // The bar: a dark track with the pet's red health bar image in it, as wide as the health.
    if (void* track = make(frame, "GuiLook/StaticImage", name("Track", k).c_str(), "{{0,4},{0,22}}", ("{{0," + std::to_string(kBarWidth) + "},{0,10}}").c_str(), ""))
    {
      prop(track, "Image", "set:WindowsLook image:Background");
      prop(track, "ImageColours", "tl:C0000000 tr:C0000000 bl:C0000000 br:C0000000");
      prop(track, "MousePassThroughEnabled", "True");
      if (void* fill = make(track, "GuiLook/StaticImage", name("Fill", k).c_str(), "{{0,1},{0,1}}", "{{1,-2},{1,-2}}", ""))
      {
        prop(fill, "Image", "set:ui2 image:PetHealthBar");
        prop(fill, "MousePassThroughEnabled", "True");
      }
    }
    char notePos[64];
    std::snprintf(notePos, sizeof(notePos), "{{0,%d},{0,18}}", kBarWidth + 10);
    if (void* note = make(frame, "GuiLook/StaticTextOutline", name("Note", k).c_str(), notePos, "{{0,140},{0,18}}", ""))
    {
      prop(note, "Font", "SerifSmall");
      prop(note, "TextColour", "FFC0C0C0");
      prop(note, "MousePassThroughEnabled", "True");
    }
  }
  for (Shown& s : g_shown)
    s = Shown{};
  TLMP_LOG("hud: party frames added");
}

const char* noteFor(const Member& m)
{
  if (m.dead)
    return "dead";
  switch (m.where)
  {
  case Member::Where::Town: return "in town";
  case Member::Where::Elsewhere: return "on another floor";
  case Member::Where::Loading: return "loading";
  default: return "";
  }
}

void show(int k, const Member* m)
{
  Shown& s = g_shown[k];
  void* frame = windowNamed(name("Frame", k).c_str());
  if (!frame)
    return;
  bool visible = m != nullptr;
  if (visible != s.visible)
  {
    s.visible = visible;
    api.setVisible(frame, visible);
  }
  if (!m)
    return;
  std::string label = m->level > 0 ? m->name + "   Lv " + std::to_string(m->level) : m->name;
  if (label != s.label)
  {
    s.label = label;
    if (void* w = windowNamed(name("Name", k).c_str()))
      text(w, label);
  }
  std::string note = noteFor(*m);
  if (note != s.note)
  {
    s.note = note;
    if (void* w = windowNamed(name("Note", k).c_str()))
      text(w, note);
  }
  int percent = m->dead || !m->maxHealth ? 0
                                         : static_cast<int>(std::lround(100.0 * m->health / m->maxHealth));
  percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
  if (percent != s.percent)
  {
    s.percent = percent;
    if (void* fill = windowNamed(name("Fill", k).c_str()))
    {
      char size[64];
      std::snprintf(size, sizeof(size), "{{%.3f,-2},{1,-2}}", percent / 100.0);
      prop(fill, "UnifiedSize", size);
      api.setVisible(fill, percent > 0);
    }
  }
  bool away = m->where != Member::Where::Here;
  if (m->dead != s.dead || away != s.away)
  {
    s.dead = m->dead;
    s.away = away;
    if (void* w = windowNamed(name("Name", k).c_str()))
      prop(w, "TextColour", m->dead ? "FF909090" : "FFFFFFFF");
    if (api.setAlpha)
      api.setAlpha(frame, away ? 0.6f : 1.0f);
  }
}

} // namespace

void tick(bool visible, const std::vector<Member>& members)
{
  if (!api.ok && !resolve())
    return;
  void* sheet = cegui::sheet();
  if (!sheet)
    return;
  void* root = windowNamed(kRoot);
  if (!root)
  {
    if (!visible)
      return;
    build(sheet);
    root = windowNamed(kRoot);
    if (!root)
      return;
  }
  // The game puts up another root window between the title screen and gameplay: move to it.
  if (api.parent(root) != sheet)
    api.addChild(sheet, root);
  if (api.visible(root, true) != visible)
    api.setVisible(root, visible);
  g_text.clear();
  if (!visible)
    return;
  for (int k = 0; k < kFrames; k++)
  {
    const Member* m = k < static_cast<int>(members.size()) ? &members[k] : nullptr;
    show(k, m);
    if (m)
      g_text += (g_text.empty() ? "" : "\n") + m->name + "|" + std::to_string(m->level) + "|" +
                std::to_string(g_shown[k].percent) + "|" + noteFor(*m);
  }
}

std::string shownText()
{
  return g_text;
}

} // namespace tlmp::hud
