#pragma once
//
// Rejoining the server without restarting the game.
//
// A gamemode under development is rebuilt and the server restarted a dozen
// times an hour, and each restart drops the client. Starting GTA again costs
// half a minute of loading; asking the client's own network layer to connect
// again costs a second, and the character is back in the world.
//
// The call goes through RakClient, which CNetGame owns, and every step of the
// way is anchored to something the client itself tells us rather than to a
// table of offsets: see "Reconnecting the client" in the README for how the
// pointer, the subobject offset and the two vtable slots were established.
//
#include "types.hpp"

namespace gtabot::samp {

// Which way back in to take. Under test: a client that reconnects but does not
// replay its own way in leaves the server with a player who joined and never
// spawned, and a gamemode is right to throw that out.
enum class Route {
  // Put CNetGame back in "waiting to connect" and let its own Process do the
  // connecting, the way it does on the way into the game.
  kState,
  // The same, with the disconnect asked for first so the server is told at
  // once rather than on the client's own terms.
  kPartThenState,
  // Call RakClient::Disconnect and RakClient::Connect directly.
  kCalls,
};

// Game thread only. Asks the client to join the server again, by the route
// given. Reports what it found and what it did; never throws. When any check
// fails it calls nothing at all and says which check it was.
json Reconnect(Route route);

}  // namespace gtabot::samp
