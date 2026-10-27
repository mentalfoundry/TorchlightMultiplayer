#pragma once

#include <cstdint>
#include <string>

// Opening the host's port on the home router with UPnP (Windows' own NAT API, natupnp.h), so
// friends can join over the internet without forwarding it by hand. Runs on its own thread
// (routers can take seconds to answer); the result is reported with status() for the host to see.
// The mapping is removed again by close(). Routers without UPnP: the host forwards UDP <port> by
// hand (see the README).
namespace tlmp::net::upnp {

enum class State
{
  Idle,
  Working,
  Open,      // mapped: externalIp is where friends connect
  NoRouter,  // no UPnP router answered (or UPnP is off on it)
  Failed,    // the router refused the mapping
};

struct Status
{
  State state = State::Idle;
  std::string externalIp; // when Open (may be empty if the router doesn't say)
  std::string detail;     // for logs
};

// Starts mapping UDP `port` to this machine (async).
void open(uint16_t port);
// Removes the mapping made by open (blocking, short).
void close();
Status status();

// Read-only check whether a UPnP router answers (no mapping made). Blocking.
bool probe(std::string* detail);

// The addresses friends use to join: this PC's on the local network (known at once), and the
// public one -- from the router when UPnP mapped the port, else asked of a public "what's my IP"
// service once (lookupPublicAddress, async). Empty while unknown.
std::string localAddressText();
void lookupPublicAddress();
std::string publicAddress();
void setPublicAddressForTest(const std::string& address); // test mode: a placeholder (no lookup)

} // namespace tlmp::net::upnp
