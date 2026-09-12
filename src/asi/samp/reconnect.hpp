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
  // Hand the session to the client's own restart path: it takes the players,
  // the local player and the pools down and leaves itself "restarting", and
  // connects out of that by itself. The whole way in is replayed, spawn
  // included, which is the only version of this the server accepts.
  kRestart,
  // Reconnect without any of that - RakClient::Disconnect and Connect,
  // nothing else. Kept because it is the useful contrast: it gets the client
  // connected just as fast and leaves the server with a player who joined and
  // never spawned.
  kCalls,
};

// Game thread only. Asks the client to join the server again, by the route
// given. Reports what it found and what it did; never throws. When any check
// fails it calls nothing at all and says which check it was.
json Reconnect(Route route);

}  // namespace gtabot::samp
