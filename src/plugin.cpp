// Entry point. Ogre loads us because Plugins.cfg lists "Plugin=TLMP"; Root calls
// dllStartPlugin() while it is being constructed, before the game creates its window.

#include "config.h"
#include "crashlog.h"
#include "game/anim.h"
#include "game/autotest.h"
#include "game/combat.h"
#include "game/floor.h"
#include "game/loot.h"
#include "game/quests.h"
#include "game/minions.h"
#include "game/minimap.h"
#include "game/skills.h"
#include "game/spawns.h"
#include "game/game.h"
#include "game/hooks.h"
#include "game/monsters.h"
#include "game/party.h"
#include "game/triggers.h"
#include "log.h"
#include "net/session.h"
#include "net/tailcat.h"
#include "net/upnp.h"
#include "menu.h"
#include "chat.h"
#include "hud.h"
#include "game/levelguard.h"
#include "ogre_abi.h"
#include "profile.h"
#include "version.h"

#include <windows.h>

namespace tlmp {

static Config g_config;
static net::Session g_session;
static ogre_abi::Root* g_root = nullptr;
static ogre_abi::RemoveFrameListenerFn g_removeFrameListener = nullptr;

constexpr float kStateSendInterval = 1.0f / 20.0f;

static void upnpReport();

static void hudTick();
static void codeJoinTick();

class NetFrameListener final : public ogre_abi::FrameListener
{
public:
  bool frameStarted(const ogre_abi::FrameEvent& evt) override
  {
    g_session.poll();
    game::setHostRole(g_session.isHost() && g_session.isConnected());
    game::tick(evt.timeSinceLastFrame);
    upnpReport();
    codeJoinTick();
    menu::tick();
    chat::tick();
    hudTick();
    game::minimap::tick(g_session.isActive());
    game::floor::tick();
    game::monsters::tick();
    game::combat::tick();
    game::loot::tick();
    game::quests::tick();
    game::spawns::tick();
    game::minions::tick();
    game::autotest::tick();
    game::triggers::tick();
    game::party::tick();

    // The session's world seed is fixed when the host first has a character, and stays for the
    // session whichever character the host plays (a new one adopts it like clients do).
    static uint32_t sessionSeed = 0;
    if (g_session.isHost())
    {
      if (!sessionSeed && (sessionSeed = game::localWorldSeed()) != 0)
      {
        TLMP_LOG("session world seed is %u", sessionSeed);
        game::onHostWorldSeed(sessionSeed);
      }
      if (sessionSeed)
        g_session.sendWorldSeed(sessionSeed);
    }

    net::PlayerInfo info = game::localPlayerInfo();
    // A session started since (from the title screen): what we "sent" before went nowhere.
    if (g_session.isConnected() != m_infoSessionUp)
    {
      m_infoSessionUp = g_session.isConnected();
      m_sentInfo = net::PlayerInfo{};
    }
    if (!(info == m_sentInfo))
    {
      m_sentInfo = info;
      g_session.sendPlayerInfo(info);
    }

    if (g_config.devMirror)
      mirrorTick();
    if (!g_config.repairPetType.empty())
      game::repairPet(g_config.repairPetType, g_config.repairPetName); // waits for Continue itself


    // Console = chat input; keep it enabled (and non-pausing) while multiplayer is on.
    m_consoleAccum += evt.timeSinceLastFrame;
    if (g_config.mode != NetMode::Off && m_consoleAccum >= 1.0f)
    {
      m_consoleAccum = 0.0f;
      game::enableConsoleForSession();
    }

    m_sendAccum += evt.timeSinceLastFrame;
    if (m_sendAccum >= kStateSendInterval && g_session.isConnected())
    {
      m_sendAccum = 0.0f;
      net::PlayerState st;
      st.tick = m_tick++;
      if (game::readLocalPlayerState(st))
        g_session.sendLocalState(st);
    }
    return true;
  }

private:
  // Feeds our own character back in as remote slot 3, offset sideways, to exercise avatar
  // spawning/equipment/facing with a single instance.
  void mirrorTick()
  {
    constexpr uint8_t kMirrorSlot = 3;
    if (!m_mirrorJoined)
    {
      m_mirrorJoined = true;
      game::onRemoteJoined(kMirrorSlot, "Mirror");
    }
    game::onRemoteInfo(kMirrorSlot, game::localPlayerInfo());
    net::PlayerState st;
    st.tick = m_mirrorTick++;
    if (game::readLocalPlayerState(st))
    {
      st.pos[0] += 2.0f;
      st.petPos[0] += 2.0f;
      game::onRemoteState(kMirrorSlot, st);
    }
  }

  float m_consoleAccum = 0.0f;
  float m_repairTimer = 0.0f;
  bool m_mirrorJoined = false;
  uint32_t m_mirrorTick = 0;
  float m_sendAccum = 0.0f;
  uint32_t m_tick = 0;
  net::PlayerInfo m_sentInfo;
  bool m_infoSessionUp = false;
};

static NetFrameListener g_frameListener;

// The party frames (hud.h): everyone else in the session, from their latest PlayerState.
static void hudTick()
{
  std::vector<hud::Member> members;
  bool show = g_session.isActive() && game::playing();
  if (show)
  {
    uint32_t here = game::localLevelId();
    for (uint8_t slot = 0; slot < net::kMaxPlayers; slot++)
    {
      const net::RemotePlayer& p = g_session.player(slot);
      if (slot == g_session.localSlot() || !p.active)
        continue;
      hud::Member m;
      m.slot = slot;
      m.name = game::displayName(slot, p.name);
      m.where = hud::Member::Where::Loading;
      if (p.hasState)
      {
        m.level = p.state.level;
        m.health = p.state.health;
        m.maxHealth = p.state.maxHealth;
        m.dead = p.state.dead;
        if (p.state.inGame && p.state.levelId)
          m.where = p.state.levelId == here                    ? hud::Member::Where::Here
                    : game::floor::isTownLevel(p.state.levelId) ? hud::Member::Where::Town
                                                                : hud::Member::Where::Elsewhere;
      }
      members.push_back(std::move(m));
    }
  }
  hud::tick(show, members);
}

static bool attachToOgre()
{
  HMODULE ogre = GetModuleHandleW(L"OgreMain.dll");
  if (!ogre)
    return false;
  auto getRoot = reinterpret_cast<ogre_abi::GetRootFn>(GetProcAddress(ogre, ogre_abi::kGetRoot));
  auto addListener = reinterpret_cast<ogre_abi::AddFrameListenerFn>(GetProcAddress(ogre, ogre_abi::kAddFrameListener));
  g_removeFrameListener =
    reinterpret_cast<ogre_abi::RemoveFrameListenerFn>(GetProcAddress(ogre, ogre_abi::kRemoveFrameListener));
  if (!getRoot || !addListener || !g_removeFrameListener)
    return false;
  g_root = getRoot();
  if (!g_root)
    return false;
  addListener(g_root, &g_frameListener);
  return true;
}

static void dispatch(uint8_t from, net::Msg type, net::Reader& r);

static net::Session::Callbacks g_callbacks;

// The name others see: the character's, unless TLMP.ini / the command line set one.
static std::string sessionName()
{
  if (g_config.playerName != "Adventurer" && !g_config.playerName.empty())
    return g_config.playerName;
  std::string character = game::localPlayerInfo().charName;
  return character.empty() ? g_config.playerName : character;
}

// The host's party settings (its own, or as received): monster health, damage and kill XP by party size.
static void applyPartyRules(const net::PartyRules& rules)
{
  game::monsters::setHealthPerPlayer(rules.healthPerPlayer);
  game::combat::setRules(rules);
  game::loot::setLootBonus(rules.lootPerPlayer);
}

// Starts hosting, or joining "host[:port]". False if a session is already running or it failed.
// A join by code waiting for Tailcat to reach the host (net/tailcat.h); the ENet session starts
// once it has (codeJoinTick).
static bool g_codeJoinPending = false;

static bool beginSession(NetMode mode, const std::string& address);

static void codeJoinTick()
{
  if (!g_codeJoinPending)
    return;
  net::tailcat::State s = net::tailcat::state();
  if (s == net::tailcat::State::Ready)
  {
    g_codeJoinPending = false;
    beginSession(NetMode::Client, "127.0.0.1:" + std::to_string(net::tailcat::localPort()));
    g_config.address = ""; // never show (or keep) the relay port as the host's address
  }
  else if (s != net::tailcat::State::Starting)
  {
    g_codeJoinPending = false;
    std::string why = net::tailcat::error();
    TLMP_LOG("session: joining by code failed: %s", why.c_str());
    game::showMessage("Could not join with that code: " + why, 8.0f);
    net::tailcat::stop();
  }
}

static bool beginSession(NetMode mode, const std::string& address)
{
  if (g_session.isActive() || g_codeJoinPending)
    return false;
  if (mode == NetMode::Client && net::tailcat::looksLikeCode(address))
  {
    // The code is a secret: not logged, not kept in TLMP.ini, not shown.
    TLMP_LOG("session: joining with a join code");
    net::tailcat::startClient(address);
    g_codeJoinPending = net::tailcat::state() == net::tailcat::State::Starting;
    if (!g_codeJoinPending)
      game::showMessage("Could not join with that code: " + net::tailcat::error(), 8.0f);
    return g_codeJoinPending;
  }
  Config c = g_config;
  c.mode = mode;
  c.playerName = sessionName();
  if (mode == NetMode::Client)
  {
    c.address = address;
    if (size_t colon = address.rfind(':'); colon != std::string::npos)
    {
      c.address = address.substr(0, colon);
      c.port = static_cast<uint16_t>(std::atoi(address.c_str() + colon + 1));
    }
    g_config.address = address;
  }
  g_session.setPartyRules(c.rules);
  if (mode == NetMode::Host)
    applyPartyRules(c.rules);
  if (!g_session.start(c, g_callbacks))
  {
    game::showMessage(mode == NetMode::Host ? "Could not start hosting (is the port in use?)" : "Could not connect.", 6.0f);
    return false;
  }
  g_config.mode = mode;
  TLMP_LOG("session: %s as '%s'", mode == NetMode::Host ? "hosting" : ("joining " + address).c_str(), c.playerName.c_str());
  // Test runs never touch the router (they host on every run) or ask for the public address.
  if (mode == NetMode::Host && c.upnp && c.testScript.empty())
    net::upnp::open(c.port);
  if (mode == NetMode::Host && c.testScript.empty())
    net::upnp::lookupPublicAddress();
  if (mode == NetMode::Host && net::tailcat::enabled())
    net::tailcat::startHost(c.port);
  return true;
}

// The session's goodbye to the others goes out through the join code's relay (a background thread):
// give it a moment before Tailcat closes, or they would take the end for a dropped connection.
static void stopTailcatAfterGoodbye()
{
  if (net::tailcat::state() == net::tailcat::State::Off)
    return;
  Sleep(300);
  net::tailcat::stop();
}

// Ends the session from this side (the others see us leave); this game goes on alone.
static void leaveSession(const char* why)
{
  if (g_codeJoinPending)
  {
    g_codeJoinPending = false;
    net::tailcat::stop();
  }
  if (!g_session.isActive())
    return;
  TLMP_LOG("session: ending (%s)", why);
  game::onHostWorldSeed(0);
  game::endSession();
  game::loot::endSession();
  game::floor::endSession();
  g_session.stop();
  net::upnp::close();
  stopTailcatAfterGoodbye();
  g_config.mode = NetMode::Off;
}

// The host's join code, for the title screen's status ("" when join codes are off).
static std::string codeStatus()
{
  if (!g_session.isHost())
    return "";
  switch (net::tailcat::state())
  {
  case net::tailcat::State::Starting: return "Getting a join code...";
  case net::tailcat::State::Ready: return "Join code ready";
  case net::tailcat::State::Failed: return "No join code (" + net::tailcat::error() + ")";
  default: return "";
  }
}

// Who a client joined: the address typed, or "the host" when it joined with a code.
static std::string hostLabel()
{
  return g_config.address.empty() ? std::string("the host") : g_config.address;
}

static std::string sessionStatus()
{
  if (g_codeJoinPending)
    return "Co-op: connecting with the join code...";
  char b[160];
  if (!g_session.isActive())
    return "Co-op: not in a session (TLMP " TLMP_VERSION ")";
  if (g_session.isHost())
  {
    // How friends join has its own big line (menu joinAddress, joinCode); this one explains.
    std::string port = std::to_string(g_config.port);
    std::string lan = net::upnp::localAddressText();
    std::string pub = net::upnp::publicAddress();
    net::upnp::Status u = net::upnp::status();
    int players = g_session.playerCount();
    std::snprintf(b, sizeof(b), "Hosting, %d player%s.", players, players == 1 ? "" : "s");
    std::string line = b;
    // Then, a line each: the join code (the easy way; it needs nothing from either router)...
    switch (net::tailcat::state())
    {
    case net::tailcat::State::Ready:
      line += "\nSend friends the join code: they paste it into Join Co-op. No router setup needed.";
      break;
    case net::tailcat::State::Starting:
      line += "\nGetting a join code...";
      break;
    case net::tailcat::State::Failed:
      line += "\n" + codeStatus() + ", but friends can still join by IP.";
      break;
    default:
      break;
    }
    // ...and joining by IP instead.
    line += net::tailcat::state() == net::tailcat::State::Off ? "\nFriends join by IP: " : "\nOr join by IP: ";
    if (!lan.empty())
      line += lan + ":" + port + " on your network. ";
    if (pub.empty())
      line += "Finding your internet address...";
    else if (u.state == net::upnp::State::Working)
      line += "Opening the port on your router...";
    else if (u.state == net::upnp::State::Open)
      line += "Your router's port is open.";
    else
      line += "You may need to forward UDP port " + port + ".";
    return line;
  }
  if (g_session.isReconnecting())
    return "Co-op: lost the connection; reconnecting to " + hostLabel() + "...";
  if (!g_session.isConnected())
    return "Co-op: connecting to " + hostLabel() + "...";
  std::snprintf(b, sizeof(b), "Co-op: joined %s, %d player(s)", hostLabel().c_str(), g_session.playerCount());
  return b;
}

static void start()
{
  log::open();
  TLMP_LOG("TLMP %s starting; game build stamp %08x", TLMP_VERSION, net::gameBuildStamp());
  TLMP_LOG("command line: %ls", GetCommandLineW());
  if (const wchar_t* dir = profile::activeProfileDir())
    TLMP_LOG("profile: user data redirected to %ls", dir);

  if (net::gameBuildStamp() != game::kExpectedBuildStamp)
  {
    TLMP_LOG("Unsupported Torchlight.exe build (expected %08x); multiplayer disabled", game::kExpectedBuildStamp);
    return;
  }

  g_config = loadConfig();
  TLMP_LOG("config: mode=%s address=%s port=%u name=%s", toString(g_config.mode), g_config.address.c_str(),
           g_config.port, g_config.playerName.c_str());

  if (!hooks::install())
  {
    TLMP_LOG("hook installation failed; multiplayer disabled");
    return;
  }

  if (g_config.devWorldSeed)
    game::onHostWorldSeed(g_config.devWorldSeed);

  if (!attachToOgre())
  {
    TLMP_LOG("could not register Ogre frame listener; multiplayer disabled");
    return;
  }

  net::Session::Callbacks& cb = g_callbacks;
  cb.onPlayerJoined = [](uint8_t slot, const std::string& name) { game::onRemoteJoined(slot, name); };
  cb.onPlayerLeft = [](uint8_t slot) {
    game::onRemoteLeft(slot);
    game::floor::onPlayerLeft(slot);
  };
  cb.onPlayerState = [](uint8_t slot, const net::PlayerState& st) { game::onRemoteState(slot, st); };
  cb.onPlayerInfo = [](uint8_t slot, const net::PlayerInfo& info) { game::onRemoteInfo(slot, info); };
  cb.onWorldSeed = [](uint32_t seed) { game::onHostWorldSeed(seed); };
  cb.onHostLost = [](net::HostLost why) {
    net::tailcat::stop(); // a client that joined with a code: its connection goes with the session
    // The session is over: this game goes on alone. What the floor shows becomes its own (copies
    // get their AI back once we're no client), others' dropped items leave, settings are ours again.
    game::endSession();
    game::loot::endSession();
    game::floor::endSession();
    switch (why)
    {
    case net::HostLost::Left:
      game::showHostLeft("The host has left the game. You are playing on your own now.");
      break;
    case net::HostLost::Dropped:
      game::showHostLeft("Could not reconnect to the host. You are playing on your own now.");
      break;
    case net::HostLost::Unreachable:
      game::showHostLeft("Could not connect to the host.");
      break;
    case net::HostLost::Rejected:
      game::showHostLeft("The host turned the connection down (game full, or a different TLMP or game version).");
      break;
    }
  };
  // A dropped connection: the others leave our screen until we're back (Session::poll retries).
  cb.onConnectionLost = [] { game::showMessage("Lost the connection to the host. Reconnecting...", 6.0f); };
  cb.onReconnected = [] {
    game::floor::onReconnected();
    game::showMessage("Reconnected to the host.", 4.0f);
  };
  cb.onPartyRules = [](const net::PartyRules& rules) { applyPartyRules(rules); };
  cb.onGameMessage = [](uint8_t from, net::Msg type, net::Reader& r) { dispatch(from, type, r); };
  game::monsters::attach(&g_session);
  game::floor::attach(&g_session);
  game::spawns::attach(&g_session);
  game::quests::attach(&g_session);
  game::skills::attach(&g_session);
  game::minions::attach(&g_session);
  game::combat::attach(&g_session);
  game::anim::attach(&g_session);
  game::loot::attach(&g_session);
  game::triggers::attach(&g_session);
  game::party::attach(&g_session);
  cb.onChat = [](uint8_t slot, const std::string& text) {
    game::showChat(game::displayName(slot, g_session.player(slot).name), text);
  };
  game::setChatSender([](const std::string& text) { g_session.sendChat(text); });
  chat::init(chat::Actions{
    [](const std::string& text) { game::say(text); },
    [] { return g_session.isActive() && game::playing(); },
  });
  game::levelguard::setDescriber(&game::describeUnit);
  // TLMP.ini / command line may start a session right away; otherwise the title screen's co-op
  // buttons do (menu.cpp).
  if (g_config.mode != NetMode::Off)
    beginSession(g_config.mode, g_config.address);
  // Join codes reach Tailscale's public relays: test runs use them only when a scenario says so.
  net::tailcat::setEnabled(g_config.tailcat && g_config.testScript.empty());
  game::autotest::start(&g_session, g_config.testScript);
  menu::init(menu::Actions{
    [] { return beginSession(NetMode::Host, ""); },
    [](const std::string& address) {
      if (!net::tailcat::looksLikeCode(address)) // a code is a secret, and new for every hosting anyway
        WritePrivateProfileStringA("net", "address", address.c_str(), ".\\TLMP.ini"); // offered next time
      return beginSession(NetMode::Client, address);
    },
    [] { leaveSession("left from the title screen"); },
    [] { return sessionStatus(); },
    [] { return g_session.isActive() || g_codeJoinPending; },
    [] { return g_config.address; },
    [] {
      std::string pub = g_session.isHost() ? net::upnp::publicAddress() : "";
      return pub.empty() ? pub : pub + ":" + std::to_string(g_config.port);
    },
    [] { return g_session.isHost() ? net::tailcat::code() : std::string(); },
  });
}

// Game-module messages, also the ones unwrapped from a Relay.
static void dispatch(uint8_t from, net::Msg type, net::Reader& r)
{
  using net::Msg;
  switch (type)
  {
    case Msg::Relay:
      game::floor::onRelay(from, r, &dispatch);
      break;
    case Msg::Location:
    case Msg::PartyFloor:
    case Msg::OwnerGrant:
    case Msg::OwnerRelease:
    case Msg::FloorSnapshot:
      game::floor::onMessage(from, type, r);
      break;
    case Msg::TriggerUse:
    case Msg::TriggerRoster:
    case Msg::TriggerRequest:
      game::triggers::onMessage(from, type, r);
      break;
    case Msg::TravelRequest:
    case Msg::TravelStatus:
    case Msg::TravelGo:
    case Msg::HostLocation:
      game::party::onMessage(from, type, r);
      break;
    case Msg::MonsterDamage:
    case Msg::PlayerDamage:
      game::combat::onMessage(from, type, r);
      break;
    case Msg::AnimPlay:
      game::anim::onMessage(from, type, r);
      break;
    case Msg::EventWait:
      game::spawns::onMessage(from, type, r);
      break;
    case Msg::TestSync:
      game::autotest::onMessage(from, type, r);
      break;
    case Msg::QuestSpawn:
      game::quests::onMessage(from, type, r);
      break;
    case Msg::SkillCast:
      game::skills::onMessage(from, type, r);
      break;
    case Msg::Minions:
      game::minions::onMessage(from, type, r);
      break;
    case Msg::ItemDrop:
    case Msg::ItemSpawn:
    case Msg::ItemClaim:
    case Msg::ItemTaken:
      game::loot::onMessage(from, type, r);
      break;
    default:
      game::monsters::onMessage(from, type, r);
      break;
  }
}

// Host: tells the player, once in the game, whether friends can reach them over the internet.
static void upnpReport()
{
  static net::upnp::State shown = net::upnp::State::Idle;
  net::upnp::Status st = net::upnp::status();
  if (st.state == shown || st.state == net::upnp::State::Working || st.state == net::upnp::State::Idle ||
      !game::playing())
    return;
  shown = st.state;
  char line[160];
  if (st.state == net::upnp::State::Open && !st.externalIp.empty())
    std::snprintf(line, sizeof(line), "Friends can join you at %s (port %u).", st.externalIp.c_str(), g_config.port);
  else if (st.state == net::upnp::State::Open)
    std::snprintf(line, sizeof(line), "Port %u is open on your router: friends can join with your public IP address.",
                  g_config.port);
  else
    std::snprintf(line, sizeof(line), "Couldn't open the port automatically: forward UDP %u on your router for friends to join.",
                  g_config.port);
  game::showMessage(line, 10.0f);
}

static void stop()
{
  game::onHostWorldSeed(0); // put the character's own world seed back before the final save
  game::endSession();       // and the player's own console settings
  game::loot::endSession(); // other players' dropped items leave the ground
  game::floor::endSession();
  g_session.stop();
  stopTailcatAfterGoodbye();
  net::upnp::close();
  if (g_root && g_removeFrameListener)
    g_removeFrameListener(g_root, &g_frameListener);
  hooks::uninstall();
  TLMP_LOG("TLMP stopped");
  log::close();
}

} // namespace tlmp

extern "C" void __cdecl dllStartPlugin()
{
  tlmp::start();
}

extern "C" void __cdecl dllStopPlugin()
{
  tlmp::stop();
}

// Imported by the OgreGUIRenderer.dll proxy purely to force TLMP.dll to load at process
// startup; the work happens in DllMain.
extern "C" void __cdecl tlmpEarlyAnchor()
{
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
  if (reason == DLL_PROCESS_ATTACH)
  {
    DisableThreadLibraryCalls(inst);
    tlmp::crashlog::install(inst);
    tlmp::profile::installEarly();
  }
  return TRUE;
}
