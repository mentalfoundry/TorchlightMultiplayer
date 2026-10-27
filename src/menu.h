#pragma once

#include <string>

// The co-op buttons on the title screen: "Host Co-op", "Join Co-op" (an address box with Connect
// and Cancel) and, in a session, "Leave Co-op", with a status line above them. Built from the
// game's own widget types (GuiLook/StandardButton, GuiLook/Editbox, GuiLook/StaticText) next to
// its Continue button; clicks are noticed by watching the buttons' pushed state each frame.
namespace tlmp::menu {

struct Actions
{
  bool (*host)();                          // start hosting
  bool (*join)(const std::string& address); // "host[:port]"
  void (*leave)();                         // end the session
  std::string (*status)();                 // one line: what the session is doing
  bool (*inSession)();                     // hosting, connected or connecting
  std::string (*lastAddress)();            // to fill the address box with
  std::string (*joinAddress)();            // host: "IP:port" friends join at ("" while unknown / not hosting)
};

void init(const Actions& actions);
void tick(); // every frame

} // namespace tlmp::menu
