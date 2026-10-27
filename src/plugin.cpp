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
#include "game/skills.h"
#include "game/spawns.h"
#include "game/game.h"
#include "game/hooks.h"
#include "game/monsters.h"
#include "game/party.h"
#include "game/triggers.h"
#include "log.h"
#include "net/session.h"
#include "ogre_abi.h"
#include "profile.h"

#include <windows.h>

namespace tlmp {

static Config g_config;
static net::Session g_session;
static ogre_abi::Root* g_root = nullptr;
static ogre_abi::RemoveFrameListenerFn g_removeFrameListener = nullptr;

constexpr float kStateSendInterval = 1.0f / 20.0f;

class NetFrameListener final : public ogre_abi::FrameListener
{
public:
  bool frameStarted(const ogre_abi::FrameEvent& evt) override
  {
    g_session.poll();
    game::setHostRole(g_session.isHost() && g_session.isConnected());
    game::tick(evt.timeSinceLastFrame);
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
};

static NetFrameListener g_frameListener;

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

static void start()
{
  log::open();
  TLMP_LOG("TLMP starting; game build stamp %08x", net::gameBuildStamp());
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

  net::Session::Callbacks cb;
  cb.onPlayerJoined = [](uint8_t slot, const std::string& name) { game::onRemoteJoined(slot, name); };
  cb.onPlayerLeft = [](uint8_t slot) {
    game::onRemoteLeft(slot);
    game::floor::onPlayerLeft(slot);
  };
  cb.onPlayerState = [](uint8_t slot, const net::PlayerState& st) { game::onRemoteState(slot, st); };
  cb.onPlayerInfo = [](uint8_t slot, const net::PlayerInfo& info) { game::onRemoteInfo(slot, info); };
  cb.onWorldSeed = [](uint32_t seed) { game::onHostWorldSeed(seed); };
  cb.onPartyRules = [](float healthPerPlayer) { game::monsters::setHealthPerPlayer(healthPerPlayer); };
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
  g_session.setPartyRules(g_config.healthPerPlayer);
  if (g_config.mode == NetMode::Host)
    game::monsters::setHealthPerPlayer(g_config.healthPerPlayer);
  g_session.start(g_config, std::move(cb));
  game::autotest::start(&g_session, g_config.testScript);
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

static void stop()
{
  game::onHostWorldSeed(0); // put the character's own world seed back before the final save
  game::endSession();       // and the player's own console settings
  game::loot::endSession(); // other players' dropped items leave the ground
  game::floor::endSession();
  g_session.stop();
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
