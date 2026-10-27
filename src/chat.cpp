#include "chat.h"
#include "cegui.h"
#include "log.h"

#include <windows.h>

#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

namespace tlmp::chat {

using namespace cegui;

namespace {

// Layout: a column of rows on the left, its bottom at 70% of the screen height (above the health
// globe), the input line just under it. Rows are single lines of the game's outlined text, so
// long messages are wrapped here, by the font's own text widths.
constexpr int kRows = 10;
constexpr int kWidth = 460;            // the column, px
constexpr const char* kBottom = "0.70"; // of the screen height
constexpr float kShowSeconds = 15.0f;   // a message stays this long...
constexpr float kFadeSeconds = 2.0f;    // ...the last of it fading out
constexpr size_t kKeep = 50;            // messages remembered
constexpr int kMaxLength = 160;         // characters in the input line

constexpr const char* kRoot = "TLMP_Chat";
constexpr const char* kBack = "TLMP_ChatBack";
constexpr const char* kInput = "TLMP_ChatInput";
constexpr const char* kHint = "TLMP_ChatHint";

struct Message
{
  std::string who, text;
  Kind kind;
  DWORD time;
};

struct Row
{
  std::string text;
  const char* colour;
  DWORD time;
};

std::mutex g_lock;
std::deque<Message> g_messages; // oldest first
bool g_dirty = true;
Actions g_actions{};
bool g_open = false;
bool g_enterHeld = false;
std::vector<Row> g_rows;   // laid out, oldest first, at most kRows
float g_alpha[kRows] = {}; // as last set on each row window
float g_lineHeight = 18.0f;

std::string rowName(int k)
{
  return "TLMP_ChatRow" + std::to_string(k);
}

const char* colourOf(Kind kind)
{
  switch (kind)
  {
  case Kind::Own: return "FFB4DCFF";    // light blue
  case Kind::System: return "FFFFC84A"; // the game's gold
  default: return "FFFFFFFF";
  }
}

void* fontOf(void* w)
{
  return w && api.font ? api.font(w, true) : nullptr;
}

float extent(void* font, const std::string& s)
{
  if (!font || !api.textExtent)
    return static_cast<float>(s.size()) * 8.0f; // a guess per character
  Str t(s);
  return api.textExtent(font, t.p(), 1.0f);
}

size_t nextCodePoint(const std::string& s, size_t i)
{
  i++;
  while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
    i++;
  return i;
}

std::string trimRight(const std::string& s)
{
  size_t e = s.find_last_not_of(' ');
  return e == std::string::npos ? "" : s.substr(0, e + 1);
}

// Splits `s` into rows no wider than `width`: at spaces, or inside a word too long for a row.
// Rows after the first are indented.
std::vector<std::string> wrap(void* font, const std::string& s, float width)
{
  static const std::string kIndent = "   ";
  std::vector<std::string> out;
  std::string line;
  size_t i = 0;
  while (i < s.size())
  {
    size_t space = s.find(' ', i);
    size_t end = space == std::string::npos ? s.size() : space + 1;
    std::string word = s.substr(i, end - i);
    i = end;
    std::string candidate = line + word;
    if (line.empty() || line == kIndent || extent(font, trimRight(candidate)) <= width)
      line = candidate;
    else
    {
      out.push_back(trimRight(line));
      line = kIndent + word;
    }
    // A word wider than a row by itself: cut it at the last character that fits.
    while (extent(font, trimRight(line)) > width)
    {
      size_t start = line.compare(0, kIndent.size(), kIndent) == 0 ? kIndent.size() : 0;
      size_t cut = nextCodePoint(line, start); // at least one character per row
      for (size_t c = cut; c < line.size(); c = nextCodePoint(line, c))
      {
        size_t next = nextCodePoint(line, c);
        if (extent(font, line.substr(0, next)) > width)
          break;
        cut = next;
      }
      if (cut >= line.size())
        break;
      out.push_back(line.substr(0, cut));
      line = kIndent + line.substr(cut);
    }
  }
  if (!trimRight(line).empty())
    out.push_back(trimRight(line));
  return out;
}

// Lays out the newest messages into rows (as many as fit).
void layout(void* font)
{
  std::deque<Message> messages;
  {
    std::lock_guard<std::mutex> l(g_lock);
    messages = g_messages;
    g_dirty = false;
  }
  std::vector<Row> rows;
  for (auto m = messages.rbegin(); m != messages.rend() && static_cast<int>(rows.size()) < kRows; ++m)
  {
    std::string line = m->kind == Kind::System ? m->text : m->who + ": " + m->text;
    std::vector<std::string> wrapped = wrap(font, line, static_cast<float>(kWidth - 8));
    for (auto r = wrapped.rbegin(); r != wrapped.rend() && static_cast<int>(rows.size()) < kRows; ++r)
      rows.push_back({*r, colourOf(m->kind), m->time});
  }
  g_rows.assign(rows.rbegin(), rows.rend());
  for (int k = 0; k < kRows; k++)
  {
    void* w = windowNamed(rowName(k).c_str());
    if (!w)
      continue;
    // Row k counts up from the bottom: the newest row is row 0.
    int index = static_cast<int>(g_rows.size()) - 1 - k;
    if (index < 0)
    {
      text(w, "");
      continue;
    }
    text(w, g_rows[index].text);
    prop(w, "TextColour", g_rows[index].colour);
  }
}

void build(void* sheet)
{
  void* root = make(sheet, "DefaultWindow", kRoot, "{{0,0},{0,0}}", "{{1,0},{1,0}}", "");
  if (!root)
    return;
  prop(root, "MousePassThroughEnabled", "True");
  prop(root, "AlwaysOnTop", "True");
  for (int k = 0; k < kRows; k++)
  {
    void* row = make(root, "GuiLook/StaticTextOutline", rowName(k).c_str(), "{{0,16},{0,0}}", "{{0,460},{0,20}}", "");
    if (!row)
      return;
    prop(row, "Font", "Serif");
    prop(row, "MousePassThroughEnabled", "True");
    api.setVisible(row, false);
    if (k == 0 && api.lineSpacing)
      if (void* font = fontOf(row))
        g_lineHeight = api.lineSpacing(font, 1.0f) + 1.0f;
  }
  int lh = static_cast<int>(g_lineHeight + 0.5f);
  for (int k = 0; k < kRows; k++)
    if (void* row = windowNamed(rowName(k).c_str()))
    {
      char pos[64], size[64];
      std::snprintf(pos, sizeof(pos), "{{0,16},{%s,%d}}", kBottom, -(k + 1) * lh - 2);
      std::snprintf(size, sizeof(size), "{{0,%d},{0,%d}}", kWidth, lh + 4);
      prop(row, "UnifiedPosition", pos);
      prop(row, "UnifiedSize", size);
    }
  // A framed field (as the join box on the title screen): a text panel under the edit box, its
  // border drawn outside its rectangle.
  char pos[64];
  std::snprintf(pos, sizeof(pos), "{{0,16},{%s,6}}", kBottom);
  if (void* back = make(root, "GuiLook/ItemText", kBack, pos, "{{0,452},{0,32}}", ""))
  {
    prop(back, "MousePassThroughEnabled", "True");
    api.setVisible(back, false);
  }
  std::snprintf(pos, sizeof(pos), "{{0,24},{%s,10}}", kBottom);
  if (void* input = make(root, "GuiLook/Editbox", kInput, pos, "{{0,436},{0,24}}", ""))
  {
    prop(input, "MaxTextLength", std::to_string(kMaxLength));
    prop(input, "Font", "Serif");
    api.setVisible(input, false);
  }
  std::snprintf(pos, sizeof(pos), "{{0,20},{%s,42}}", kBottom);
  if (void* hint = make(root, "GuiLook/StaticTextOutline", kHint, pos, "{{0,452},{0,18}}", "Enter: send     Esc: cancel"))
  {
    prop(hint, "Font", "SerifSmall");
    prop(hint, "TextColour", "FFB0B0B0");
    prop(hint, "MousePassThroughEnabled", "True");
    api.setVisible(hint, false);
  }
  for (float& a : g_alpha)
    a = -1.0f;
  g_dirty = true;
  TLMP_LOG("chat: chat box added (rows %d px)", lh);
}

void showInput(bool show)
{
  for (const char* n : {kBack, kInput, kHint})
    if (void* w = windowNamed(n))
      api.setVisible(w, show);
  if (void* input = windowNamed(kInput))
  {
    text(input, "");
    if (show && api.activate)
      api.activate(input);
    if (!show && api.deactivate)
      api.deactivate(input);
  }
}

void open()
{
  g_open = true;
  showInput(true);
  if (void* root = windowNamed(kRoot); root && api.moveToFront)
    api.moveToFront(root);
}

void close()
{
  g_open = false;
  showInput(false);
}

void submit()
{
  std::string typed;
  for (char c : textOf(windowNamed(kInput)))
    if (static_cast<unsigned char>(c) >= 0x20)
      typed.push_back(c);
  size_t b = typed.find_first_not_of(' ');
  typed = b == std::string::npos ? "" : trimRight(typed.substr(b));
  close();
  if (!typed.empty() && g_actions.send)
    g_actions.send(typed);
}

// Something else is being typed in: the developer console, the character name box ...
bool otherEditboxActive()
{
  void* sheet = cegui::sheet();
  void* active = sheet && api.activeChild ? api.activeChild(sheet) : nullptr;
  return active && active != windowNamed(kInput) && api.visible(active, false) &&
         typeOf(active).find("Editbox") != std::string::npos;
}

bool canChat()
{
  return api.ok && g_actions.canChat && g_actions.canChat() && windowNamed(kRoot);
}

} // namespace

void init(const Actions& actions)
{
  g_actions = actions;
}

void add(const std::string& who, const std::string& text, Kind kind)
{
  std::lock_guard<std::mutex> l(g_lock);
  g_messages.push_back({who, text, kind, GetTickCount()});
  while (g_messages.size() > kKeep)
    g_messages.pop_front();
  g_dirty = true;
}

void tick()
{
  if (!api.ok && !resolve())
    return;
  void* sheet = cegui::sheet();
  if (!sheet)
    return;
  void* root = windowNamed(kRoot);
  if (!root)
  {
    build(sheet);
    root = windowNamed(kRoot);
    if (!root)
      return;
  }
  // The game puts up another root window between the title screen and gameplay: move to it.
  if (api.parent(root) != sheet)
    api.addChild(sheet, root);
  bool show = g_actions.canChat && g_actions.canChat();
  if (api.visible(root, true) != show)
    api.setVisible(root, show);
  if (!show)
  {
    if (g_open)
      close();
    return;
  }
  if (g_dirty)
    layout(fontOf(windowNamed(rowName(0).c_str())));
  if (g_open)
    if (void* input = windowNamed(kInput); input && api.isActive && api.activate && !api.isActive(input))
      api.activate(input); // a click on the game took the focus away
  DWORD now = GetTickCount();
  for (int k = 0; k < kRows; k++)
  {
    void* w = windowNamed(rowName(k).c_str());
    if (!w)
      continue;
    int index = static_cast<int>(g_rows.size()) - 1 - k;
    float alpha = 0.0f;
    if (index >= 0)
    {
      float age = static_cast<float>(now - g_rows[index].time) / 1000.0f;
      alpha = g_open ? 1.0f : age < kShowSeconds - kFadeSeconds ? 1.0f : (kShowSeconds - age) / kFadeSeconds;
      if (alpha < 0.0f)
        alpha = 0.0f;
    }
    if (alpha == g_alpha[k])
      continue;
    g_alpha[k] = alpha;
    api.setVisible(w, alpha > 0.0f);
    if (api.setAlpha && alpha > 0.0f)
      api.setAlpha(w, alpha);
  }
}

bool onKey(uint32_t msg, uint32_t vk)
{
  const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
  const bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
  if (vk == VK_RETURN)
  {
    if (up)
    {
      g_enterHeld = false;
      return false;
    }
    if (!down)
      return false;
    bool repeat = g_enterHeld;
    g_enterHeld = true;
    if (g_open)
    {
      if (!repeat)
        submit();
      return true;
    }
    if (!repeat && canChat() && !otherEditboxActive())
    {
      open();
      return true;
    }
    return false;
  }
  if (!g_open)
    return false;
  if (down && vk == VK_ESCAPE)
  {
    close();
    return true;
  }
  return down; // typing: the game sees no key go down (ups go through: nothing stays held)
}

bool typing()
{
  return g_open;
}

std::string shownText()
{
  std::string out;
  for (const Row& r : g_rows)
    out += (out.empty() ? "" : "\n") + r.text;
  return out;
}

} // namespace tlmp::chat
