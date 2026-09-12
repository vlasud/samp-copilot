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

// Game thread only. Asks the client to disconnect (if it still holds a
// connection) and connect again to the address the launcher was given.
// Reports what it found and what it did; never throws. When any check fails
// it calls nothing at all and says which check it was.
json Reconnect();

}  // namespace gtabot::samp
