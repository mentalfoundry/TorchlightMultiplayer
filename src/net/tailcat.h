#pragma once

#include <cstdint>
#include <string>

// Join codes: playing over Tailscale's Tailcat (libtailcat.dll next to Torchlight.exe, built by
// tools\tailcat\build.ps1). The host's game gets a code (a Tailcat address: its keys and relay);
// a friend pastes it to join. Tailcat finds a direct path through both routers (NAT traversal) or
// relays through Tailscale's public DERP servers, WireGuard-encrypted -- no accounts, no router
// settings. ENet is unchanged: the host's Tailcat server forwards each friend's UDP flow to the
// host's ENet port, and a joining client's ENet talks to a local relay port that forwards to its
// flow. Datagrams over Tailcat are at most 1232 bytes; the session uses a 1200-byte ENet MTU.
//
// The code is a secret (anyone with it can join): it is never logged. The IP route (UPnP or a
// forwarded port) stays as the alternative, e.g. should the public relays go away.
namespace tlmp::net::tailcat {

constexpr uint16_t kServicePort = 27960; // the UDP port joining clients dial on the host's Tailcat server

enum class State
{
  Off,
  Starting, // host: getting a code; client: connecting to the code's host
  Ready,    // host: code() is set; client: localPort() reaches the host
  Failed,
};

bool available();      // libtailcat.dll loads (and speaks ABI 1)
// Whether hosting offers a code ([net] tailcat; off in test runs unless a scenario turns it on).
void setEnabled(bool on);
bool enabled();
State state();
std::string error();   // why it Failed (no secrets in it)

// Host: start a Tailcat server forwarding joiners to this machine's ENet port (async).
void startHost(uint16_t enetPort);
std::string code();    // host, once Ready; empty otherwise

// Client: connect to a code's host (async). ENet should connect to 127.0.0.1:localPort() once
// state() is Ready.
void startClient(const std::string& code);
uint16_t localPort();

void stop();           // closes everything (host or client)

// A pasted or typed join code (as opposed to an IP address or host name).
bool looksLikeCode(const std::string& text);

} // namespace tlmp::net::tailcat
