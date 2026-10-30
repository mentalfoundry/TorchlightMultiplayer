#pragma once

#include "config.h"
#include "net/protocol.h"

#include <array>
#include <functional>
#include <string>

struct _ENetHost;
struct _ENetPeer;

namespace tlmp::net {

struct RemotePlayer
{
  bool active = false;
  std::string name;
  PlayerInfo info; // empty until the player's PlayerInfo arrives
  PlayerState state;
  bool hasState = false;
  _ENetPeer* peer = nullptr; // host only: the connection for this slot
};

// Owns the ENet host/client. Single-threaded by design: poll() is called from the
// game's main thread once per frame, so every callback runs where touching game
// objects is safe.
// Why a client's session with its host ended.
enum class HostLost
{
  Left,        // the host ended the session (left or quit)
  Dropped,     // the connection was lost and could not be got back
  Unreachable, // never connected
  Rejected,    // the host refused us (full, different version or game build)
};

class Session
{
public:
  struct Callbacks
  {
    std::function<void(uint8_t slot, const std::string& name)> onPlayerJoined;
    std::function<void(uint8_t slot)> onPlayerLeft;
    std::function<void(uint8_t slot, const PlayerState& state)> onPlayerState;
    std::function<void(uint8_t slot, const std::string& text)> onChat;
    std::function<void(uint8_t slot, const PlayerInfo& info)> onPlayerInfo;
    std::function<void(uint32_t seed)> onWorldSeed; // client only; 0 = host gone
    std::function<void(HostLost why)> onHostLost;    // client only: the session is over
    // Client only: the connection dropped; the host is being reconnected to (for up to a minute).
    // Everyone else has left (onPlayerLeft) and comes back after onReconnected.
    std::function<void()> onConnectionLost;
    std::function<void()> onReconnected;
    std::function<void(const PartyRules& rules)> onPartyRules; // client only: the host's party settings
    // Message types the session itself doesn't handle, for game modules. The reader is
    // positioned just after the type byte. On a client, fromSlot is always 0 (the host).
    std::function<void(uint8_t fromSlot, Msg type, Reader& r)> onGameMessage;
  };

  bool start(const Config& cfg, Callbacks callbacks);
  void stop();
  void poll();

  void sendLocalState(const PlayerState& state);
  void sendChat(const std::string& text);
  // Call when the local character's class or pet is known or changes.
  void sendPlayerInfo(const PlayerInfo& info);
  // Game-module messages. Host: to one client / all clients. Client: only the host (slot 0)
  // can be addressed. Unreliable messages go on the state channel.
  void sendTo(uint8_t slot, const Writer& w, bool reliable);
  void sendToAll(const Writer& w, bool reliable);

  // Host only: call when the host character's world seed is known or changes.
  void sendWorldSeed(uint32_t seed);
  // Host: party settings every client follows (sent along with the world seed).
  void setPartyRules(const PartyRules& rules) { m_rules = rules; }

  bool isHost() const { return m_mode == NetMode::Host; }
  bool isConnected() const { return m_localSlot != kInvalidSlot; }
  bool isActive() const { return m_host != nullptr; } // hosting, connected or still connecting
  bool isReconnecting() const { return m_reconnecting; }
  // Test only: this machine receives nothing for `seconds` (as if its network went away).
  static void dropIncomingFor(float seconds);
  int playerCount() const
  {
    int n = 0;
    for (const auto& p : m_players)
      n += p.active ? 1 : 0;
    return n;
  }
  uint8_t localSlot() const { return m_localSlot; }
  const RemotePlayer& player(uint8_t slot) const { return m_players[slot]; }

private:
  void handleConnect(_ENetPeer* peer);
  void handleDisconnect(_ENetPeer* peer, uint32_t data);
  void giveUp(HostLost why);
  void dropSlot(uint8_t slot);
  void handlePacket(_ENetPeer* peer, const uint8_t* data, size_t size);

  void hostOnHello(_ENetPeer* peer, Reader& r);
  void reject(_ENetPeer* peer, RejectReason reason);
  void applyPlayerInfo(uint8_t slot, const PlayerInfo& info);
  static Writer makePlayerInfo(uint8_t slot, const PlayerInfo& info);
  uint8_t slotForPeer(_ENetPeer* peer) const;

  void send(_ENetPeer* peer, const Writer& w, Channel chan, bool reliable);
  void broadcast(const Writer& w, Channel chan, bool reliable, uint8_t exceptSlot);

  NetMode m_mode = NetMode::Off;
  std::string m_localName;
  PlayerInfo m_localInfo;
  uint32_t m_worldSeed = 0; // host: last announced; 0 = unknown
  PartyRules m_rules;
  _ENetHost* m_host = nullptr;
  _ENetPeer* m_server = nullptr; // client only
  // Client: the host's address, a token for this game run (Hello), and reconnecting after a drop.
  uint32_t m_serverHost = 0;
  uint16_t m_serverPort = 0;
  uint32_t m_token = 0;
  bool m_reconnecting = false;
  bool m_rejected = false;
  uint32_t m_reconnectUntil = 0, m_nextAttempt = 0; // GetTickCount
  // Host: the token each slot was last taken with.
  std::array<uint32_t, kMaxPlayers> m_slotTokens{};
  uint8_t m_localSlot = kInvalidSlot;
  std::array<RemotePlayer, kMaxPlayers> m_players{};
  Callbacks m_cb;
};

// Build stamp of the running Torchlight.exe; peers must match exactly because all
// hooks are address-based.
uint32_t gameBuildStamp();

} // namespace tlmp::net
