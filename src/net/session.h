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
    std::function<void()> onHostLost;               // client only: the host left or dropped
    std::function<void(float healthPerPlayer)> onPartyRules; // client only: the host's party settings
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
  void setPartyRules(float healthPerPlayer) { m_healthPerPlayer = healthPerPlayer; }

  bool isHost() const { return m_mode == NetMode::Host; }
  bool isConnected() const { return m_localSlot != kInvalidSlot; }
  bool isActive() const { return m_host != nullptr; } // hosting, connected or still connecting
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
  void handleDisconnect(_ENetPeer* peer);
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
  float m_healthPerPlayer = 0.5f;
  _ENetHost* m_host = nullptr;
  _ENetPeer* m_server = nullptr; // client only
  uint8_t m_localSlot = kInvalidSlot;
  std::array<RemotePlayer, kMaxPlayers> m_players{};
  Callbacks m_cb;
};

// Build stamp of the running Torchlight.exe; peers must match exactly because all
// hooks are address-based.
uint32_t gameBuildStamp();

} // namespace tlmp::net
