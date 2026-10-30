#include "net/session.h"
#include "log.h"

#include <enet/enet.h>
#include <windows.h>

namespace tlmp::net {

namespace {

constexpr uint32_t kReconnectFor = 60000;   // ms after a drop: then the session is over
constexpr uint32_t kFirstAttempt = 1000;    // ms after the drop
constexpr uint32_t kAttemptInterval = 3000; // ms between attempts
// A peer that doesn't acknowledge for this long is gone (ENet's defaults are 5 s / 30 s). Loading a
// floor blocks the game thread for seconds, with nothing polled; when both ends had just loaded,
// the 5 s minimum dropped connections that were fine.
constexpr uint32_t kTimeoutMinimum = 20000;
constexpr uint32_t kTimeoutMaximum = 45000;

uint32_t g_dropIncomingUntil = 0; // test: GetTickCount until which nothing is received

int ENET_CALLBACK interceptIncoming(ENetHost*, ENetEvent*)
{
  return g_dropIncomingUntil && static_cast<int32_t>(g_dropIncomingUntil - GetTickCount()) > 0 ? 1 : 0;
}

} // namespace

static void writeRules(Writer& w, const PartyRules& rules)
{
  w.f32(rules.healthPerPlayer);
  w.f32(rules.damagePerPlayer);
  w.u8(static_cast<uint8_t>(rules.experience));
  w.u8(rules.levelGap ? 1 : 0);
  w.f32(rules.lootPerPlayer);
}

static PartyRules readRules(Reader& r)
{
  PartyRules rules;
  rules.healthPerPlayer = r.f32();
  rules.damagePerPlayer = r.f32();
  rules.experience = static_cast<ExperienceModel>(r.u8());
  rules.levelGap = r.u8() != 0;
  rules.lootPerPlayer = r.f32();
  return rules;
}

void Session::dropIncomingFor(float seconds)
{
  g_dropIncomingUntil = GetTickCount() + static_cast<uint32_t>(seconds * 1000.0f);
  TLMP_LOG("net: [test] receiving nothing for %.0f s", seconds);
}

uint32_t gameBuildStamp()
{
  auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  return nt->FileHeader.TimeDateStamp;
}

bool Session::start(const Config& cfg, Callbacks callbacks)
{
  if (cfg.mode == NetMode::Off)
    return false;

  if (enet_initialize() != 0)
  {
    TLMP_LOG("net: enet_initialize failed");
    return false;
  }

  m_mode = cfg.mode;
  m_localName = cfg.playerName;
  m_cb = std::move(callbacks);

  if (m_mode == NetMode::Host)
  {
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = cfg.port;
    m_host = enet_host_create(&addr, kMaxPlayers - 1, kChanCount, 0, 0);
    if (!m_host)
    {
      TLMP_LOG("net: failed to listen on port %u", cfg.port);
      return false;
    }
    m_slotTokens = {};
    m_localSlot = 0;
    m_players[0].active = true;
    m_players[0].name = m_localName;
    TLMP_LOG("net: hosting on port %u as '%s'", cfg.port, m_localName.c_str());
  }
  else
  {
    m_host = enet_host_create(nullptr, 1, kChanCount, 0, 0);
    if (!m_host)
    {
      TLMP_LOG("net: failed to create client host");
      return false;
    }
    ENetAddress addr{};
    if (enet_address_set_host(&addr, cfg.address.c_str()) != 0)
    {
      TLMP_LOG("net: cannot resolve '%s'", cfg.address.c_str());
      return false;
    }
    addr.port = cfg.port;
    m_serverHost = addr.host;
    m_serverPort = addr.port;
    m_token = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
    if (!m_token)
      m_token = 1;
    m_reconnecting = false;
    m_rejected = false;
    m_server = enet_host_connect(m_host, &addr, kChanCount, 0);
    TLMP_LOG("net: connecting to %s:%u as '%s'", cfg.address.c_str(), cfg.port, m_localName.c_str());
  }
  return true;
}

void Session::stop()
{
  if (!m_host)
    return;
  for (size_t i = 0; i < m_host->peerCount; i++)
    enet_peer_disconnect_now(&m_host->peers[i], kGoodbye);
  enet_host_flush(m_host);
  enet_host_destroy(m_host);
  m_host = nullptr;
  m_server = nullptr;
  m_localSlot = kInvalidSlot;
  m_players = {};
  m_mode = NetMode::Off;
  m_reconnecting = false;
  enet_deinitialize();
}

void Session::poll()
{
  if (!m_host)
    return;
  m_host->intercept = interceptIncoming;

  // After a drop: try the host again every few seconds (a connect that fails comes back as a
  // disconnect, handleDisconnect).
  if (m_reconnecting && !m_server && static_cast<int32_t>(GetTickCount() - m_nextAttempt) >= 0)
  {
    ENetAddress addr{};
    addr.host = m_serverHost;
    addr.port = m_serverPort;
    m_server = enet_host_connect(m_host, &addr, kChanCount, 0);
    TLMP_LOG("net: reconnecting to the host (%lu s left)",
             static_cast<unsigned long>((m_reconnectUntil - GetTickCount()) / 1000));
    if (!m_server)
      m_nextAttempt = GetTickCount() + kAttemptInterval;
  }

  ENetEvent ev;
  while (enet_host_service(m_host, &ev, 0) > 0)
  {
    switch (ev.type)
    {
      case ENET_EVENT_TYPE_CONNECT:
        handleConnect(ev.peer);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        handleDisconnect(ev.peer, ev.data);
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        handlePacket(ev.peer, ev.packet->data, ev.packet->dataLength);
        enet_packet_destroy(ev.packet);
        break;
      default:
        break;
    }
  }
}

void Session::handleConnect(ENetPeer* peer)
{
  enet_peer_timeout(peer, 0, kTimeoutMinimum, kTimeoutMaximum);
  if (m_mode == NetMode::Client)
  {
    TLMP_LOG("net: connected to host, sending hello");
    Writer w(Msg::Hello);
    w.u32(kMagic);
    w.u16(kProtocolVersion);
    w.u32(gameBuildStamp());
    w.str(m_localName);
    w.u32(m_token);
    send(peer, w, kChanReliable, true);
  }
  // Host: wait for Hello before assigning a slot.
  peer->data = nullptr;
}

void Session::handleDisconnect(ENetPeer* peer, uint32_t data)
{
  if (m_mode == NetMode::Client)
  {
    bool wasConnected = m_localSlot != kInvalidSlot;
    for (uint8_t s = 0; s < kMaxPlayers; s++)
    {
      if (m_players[s].active && s != m_localSlot && m_cb.onPlayerLeft)
        m_cb.onPlayerLeft(s);
    }
    m_players = {};
    m_localSlot = kInvalidSlot;
    m_server = nullptr;
    if (data == kGoodbye)
    {
      TLMP_LOG("net: the host ended the session");
      giveUp(HostLost::Left);
    }
    else if (m_rejected)
      giveUp(HostLost::Rejected);
    else if (wasConnected)
    {
      // Lost, not ended: keep the host's world (seed, floors) and try to get back in.
      TLMP_LOG("net: lost the connection to the host; reconnecting for up to %u s", kReconnectFor / 1000);
      m_reconnecting = true;
      m_reconnectUntil = GetTickCount() + kReconnectFor;
      m_nextAttempt = GetTickCount() + kFirstAttempt;
      if (m_cb.onConnectionLost)
        m_cb.onConnectionLost();
    }
    else if (m_reconnecting && static_cast<int32_t>(GetTickCount() - m_reconnectUntil) < 0)
      m_nextAttempt = GetTickCount() + kAttemptInterval; // that attempt failed; another soon
    else if (m_reconnecting)
    {
      TLMP_LOG("net: could not reconnect to the host");
      giveUp(HostLost::Dropped);
    }
    else
    {
      TLMP_LOG("net: could not connect to the host");
      giveUp(HostLost::Unreachable);
    }
    return;
  }

  uint8_t slot = slotForPeer(peer);
  if (slot == kInvalidSlot)
    return;
  dropSlot(slot);
}

void Session::giveUp(HostLost why)
{
  m_reconnecting = false;
  if (m_cb.onWorldSeed)
    m_cb.onWorldSeed(0); // no host any more: fall back to our own world
  if (m_cb.onHostLost)
    m_cb.onHostLost(why);
}

// Host: a player is gone (left, timed out, or replaced by their own reconnection).
void Session::dropSlot(uint8_t slot)
{
  TLMP_LOG("net: slot %u ('%s') left", slot, m_players[slot].name.c_str());
  m_players[slot] = {};
  if (m_cb.onPlayerLeft)
    m_cb.onPlayerLeft(slot);

  Writer w(Msg::PlayerLeft);
  w.u8(slot);
  broadcast(w, kChanReliable, true, kInvalidSlot);
}

void Session::handlePacket(ENetPeer* peer, const uint8_t* data, size_t size)
{
  Reader r(data, size);
  auto type = static_cast<Msg>(r.u8());

  if (m_mode == NetMode::Host)
  {
    if (type == Msg::Hello)
    {
      hostOnHello(peer, r);
      return;
    }
    uint8_t slot = slotForPeer(peer);
    if (slot == kInvalidSlot)
      return; // not handshaken yet

    switch (type)
    {
      case Msg::PlayerState:
      {
        r.u8(); // slot claimed by the client is ignored; the host knows who sent it
        PlayerState st = readState(r);
        if (!r.ok())
          return;
        m_players[slot].state = st;
        m_players[slot].hasState = true;
        if (m_cb.onPlayerState)
          m_cb.onPlayerState(slot, st);

        Writer w(Msg::PlayerState);
        w.u8(slot);
        writeState(w, st);
        broadcast(w, kChanState, false, slot);
        break;
      }
      case Msg::PlayerInfo:
      {
        r.u8();
        PlayerInfo info = readInfo(r);
        if (!r.ok())
          return;
        applyPlayerInfo(slot, info);
        broadcast(makePlayerInfo(slot, info), kChanReliable, true, slot);
        break;
      }
      case Msg::Chat:
      {
        r.u8();
        std::string text = r.str();
        if (!r.ok())
          return;
        if (m_cb.onChat)
          m_cb.onChat(slot, text);
        Writer w(Msg::Chat);
        w.u8(slot);
        w.str(text);
        broadcast(w, kChanReliable, true, slot);
        break;
      }
      default:
        if (m_cb.onGameMessage)
          m_cb.onGameMessage(slot, type, r);
        break;
    }
    return;
  }

  // Client
  switch (type)
  {
    case Msg::Welcome:
    {
      m_localSlot = r.u8();
      uint8_t count = r.u8();
      for (uint8_t i = 0; i < count && r.ok(); i++)
      {
        uint8_t s = r.u8();
        std::string name = r.str();
        if (s >= kMaxPlayers)
          continue;
        m_players[s].active = true;
        m_players[s].name = name;
        if (s != m_localSlot && m_cb.onPlayerJoined)
          m_cb.onPlayerJoined(s, name);
      }
      TLMP_LOG("net: welcomed as slot %u", m_localSlot);
      if (!m_localInfo.className.empty())
        sendPlayerInfo(m_localInfo); // became known before we were welcomed
      if (m_reconnecting)
      {
        m_reconnecting = false;
        TLMP_LOG("net: reconnected to the host");
        if (m_cb.onReconnected)
          m_cb.onReconnected();
      }
      break;
    }
    case Msg::Reject:
    {
      auto reason = static_cast<RejectReason>(r.u8());
      TLMP_LOG("net: rejected by host (reason %u)", static_cast<unsigned>(reason));
      m_rejected = true;
      break;
    }
    case Msg::PlayerJoined:
    {
      uint8_t s = r.u8();
      std::string name = r.str();
      if (!r.ok() || s >= kMaxPlayers)
        return;
      m_players[s].active = true;
      m_players[s].name = name;
      TLMP_LOG("net: slot %u ('%s') joined", s, name.c_str());
      if (m_cb.onPlayerJoined)
        m_cb.onPlayerJoined(s, name);
      break;
    }
    case Msg::PlayerLeft:
    {
      uint8_t s = r.u8();
      if (!r.ok() || s >= kMaxPlayers)
        return;
      m_players[s] = {};
      if (m_cb.onPlayerLeft)
        m_cb.onPlayerLeft(s);
      break;
    }
    case Msg::PlayerState:
    {
      uint8_t s = r.u8();
      PlayerState st = readState(r);
      if (!r.ok() || s >= kMaxPlayers || s == m_localSlot)
        return;
      m_players[s].state = st;
      m_players[s].hasState = true;
      if (m_cb.onPlayerState)
        m_cb.onPlayerState(s, st);
      break;
    }
    case Msg::Chat:
    {
      uint8_t s = r.u8();
      std::string text = r.str();
      if (r.ok() && s < kMaxPlayers && m_cb.onChat)
        m_cb.onChat(s, text);
      break;
    }
    case Msg::WorldSeed:
    {
      uint32_t seed = r.u32();
      if (r.ok() && m_cb.onWorldSeed)
        m_cb.onWorldSeed(seed);
      PartyRules rules = readRules(r);
      if (r.ok() && m_cb.onPartyRules)
        m_cb.onPartyRules(rules);
      break;
    }
    case Msg::PlayerInfo:
    {
      uint8_t s = r.u8();
      PlayerInfo info = readInfo(r);
      if (r.ok() && s < kMaxPlayers && s != m_localSlot)
        applyPlayerInfo(s, info);
      break;
    }
    default:
      if (m_cb.onGameMessage)
        m_cb.onGameMessage(0, type, r);
      break;
  }
}

void Session::hostOnHello(ENetPeer* peer, Reader& r)
{
  uint32_t magic = r.u32();
  uint16_t version = r.u16();
  uint32_t stamp = r.u32();
  std::string name = r.str();
  uint32_t token = r.u32();
  if (!r.ok() || magic != kMagic || version != kProtocolVersion)
  {
    reject(peer, RejectReason::BadVersion);
    return;
  }
  if (stamp != gameBuildStamp())
  {
    reject(peer, RejectReason::BadGameBuild);
    return;
  }

  // A player coming back after a drop gets their old slot. If the host hasn't noticed the drop yet,
  // the old connection is let go of first.
  uint8_t slot = kInvalidSlot;
  for (uint8_t s = 1; s < kMaxPlayers && token; s++)
  {
    if (m_slotTokens[s] != token)
      continue;
    if (m_players[s].active && m_players[s].peer && m_players[s].peer != peer)
    {
      TLMP_LOG("net: '%s' is back on a new connection; dropping the old one", m_players[s].name.c_str());
      ENetPeer* old = m_players[s].peer;
      old->data = nullptr;
      dropSlot(s);
      enet_peer_reset(old);
    }
    if (!m_players[s].active)
      slot = s;
    break;
  }
  // Otherwise a free slot, preferring ones nobody has had (theirs may come back).
  for (int pass = 0; pass < 2 && slot == kInvalidSlot; pass++)
    for (uint8_t s = 1; s < kMaxPlayers; s++)
      if (!m_players[s].active && (pass == 1 || m_slotTokens[s] == 0))
      {
        slot = s;
        break;
      }
  if (slot == kInvalidSlot)
  {
    reject(peer, RejectReason::Full);
    return;
  }

  if (name.empty())
    name = "Player" + std::to_string(slot + 1);
  m_players[slot].active = true;
  m_players[slot].name = name;
  m_players[slot].peer = peer;
  m_slotTokens[slot] = token;
  peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(slot + 1));
  TLMP_LOG("net: '%s' joined as slot %u", name.c_str(), slot);

  Writer welcome(Msg::Welcome);
  welcome.u8(slot);
  uint8_t count = 0;
  for (const auto& p : m_players)
    count += p.active ? 1 : 0;
  welcome.u8(count);
  for (uint8_t s = 0; s < kMaxPlayers; s++)
  {
    if (!m_players[s].active)
      continue;
    welcome.u8(s);
    welcome.str(m_players[s].name);
  }
  send(peer, welcome, kChanReliable, true);
  if (m_worldSeed)
  {
    Writer ws(Msg::WorldSeed);
    ws.u32(m_worldSeed);
    writeRules(ws, m_rules);
    send(peer, ws, kChanReliable, true);
  }
  for (uint8_t s = 0; s < kMaxPlayers; s++)
  {
    if (s != slot && m_players[s].active && !m_players[s].info.className.empty())
      send(peer, makePlayerInfo(s, m_players[s].info), kChanReliable, true);
  }

  Writer joined(Msg::PlayerJoined);
  joined.u8(slot);
  joined.str(name);
  broadcast(joined, kChanReliable, true, slot);

  if (m_cb.onPlayerJoined)
    m_cb.onPlayerJoined(slot, name);
}

void Session::reject(ENetPeer* peer, RejectReason reason)
{
  TLMP_LOG("net: rejecting peer (reason %u)", static_cast<unsigned>(reason));
  Writer w(Msg::Reject);
  w.u8(static_cast<uint8_t>(reason));
  send(peer, w, kChanReliable, true);
  enet_peer_disconnect_later(peer, 0);
}

uint8_t Session::slotForPeer(ENetPeer* peer) const
{
  auto v = reinterpret_cast<uintptr_t>(peer->data);
  return v == 0 ? kInvalidSlot : static_cast<uint8_t>(v - 1);
}

void Session::sendLocalState(const PlayerState& state)
{
  if (!isConnected())
    return;
  Writer w(Msg::PlayerState);
  w.u8(m_localSlot);
  writeState(w, state);
  if (m_mode == NetMode::Host)
    broadcast(w, kChanState, false, 0);
  else
    send(m_server, w, kChanState, false);
}

void Session::sendChat(const std::string& text)
{
  if (!isConnected())
    return;
  Writer w(Msg::Chat);
  w.u8(m_localSlot);
  w.str(text);
  if (m_mode == NetMode::Host)
    broadcast(w, kChanReliable, true, 0);
  else
    send(m_server, w, kChanReliable, true);
}

void Session::sendPlayerInfo(const PlayerInfo& info)
{
  m_localInfo = info;
  if (!isConnected())
    return; // re-sent on Welcome
  m_players[m_localSlot].info = info;
  Writer w = makePlayerInfo(m_localSlot, info);
  if (m_mode == NetMode::Host)
    broadcast(w, kChanReliable, true, 0);
  else
    send(m_server, w, kChanReliable, true);
}

void Session::sendTo(uint8_t slot, const Writer& w, bool reliable)
{
  Channel chan = reliable ? kChanReliable : kChanState;
  if (m_mode == NetMode::Host)
  {
    if (slot < kMaxPlayers && slot != 0 && m_players[slot].active)
      send(m_players[slot].peer, w, chan, reliable);
  }
  else if (slot == 0)
  {
    send(m_server, w, chan, reliable);
  }
}

void Session::sendToAll(const Writer& w, bool reliable)
{
  Channel chan = reliable ? kChanReliable : kChanState;
  if (m_mode == NetMode::Host)
    broadcast(w, chan, reliable, kInvalidSlot);
  else
    send(m_server, w, chan, reliable);
}

void Session::sendWorldSeed(uint32_t seed)
{
  if (m_mode != NetMode::Host || seed == m_worldSeed)
    return;
  m_worldSeed = seed;
  TLMP_LOG("net: announcing world seed %u", seed);
  Writer w(Msg::WorldSeed);
  w.u32(seed);
  writeRules(w, m_rules);
  broadcast(w, kChanReliable, true, kInvalidSlot);
}

Writer Session::makePlayerInfo(uint8_t slot, const PlayerInfo& info)
{
  Writer w(Msg::PlayerInfo);
  w.u8(slot);
  writeInfo(w, info);
  return w;
}

void Session::applyPlayerInfo(uint8_t slot, const PlayerInfo& info)
{
  if (m_players[slot].info == info)
    return;
  m_players[slot].info = info;
  TLMP_LOG("net: slot %u is '%s' the %s with %s '%s'", slot, info.charName.c_str(), info.className.c_str(),
           info.petType.c_str(), info.petName.c_str());
  if (m_cb.onPlayerInfo)
    m_cb.onPlayerInfo(slot, info);
}

void Session::send(ENetPeer* peer, const Writer& w, Channel chan, bool reliable)
{
  if (!peer)
    return;
  ENetPacket* pkt = enet_packet_create(w.data(), w.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
  enet_peer_send(peer, chan, pkt);
}

void Session::broadcast(const Writer& w, Channel chan, bool reliable, uint8_t exceptSlot)
{
  for (uint8_t s = 1; s < kMaxPlayers; s++)
  {
    if (s == exceptSlot || !m_players[s].active || !m_players[s].peer)
      continue;
    send(m_players[s].peer, w, chan, reliable);
  }
}

} // namespace tlmp::net
