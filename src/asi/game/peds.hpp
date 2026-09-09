#pragma once
//
// The people the game itself is holding, and which way each is facing.
//
// SA-MP's player pool has the other players in it, and nothing else. The
// shopkeeper behind a counter, the duty doctor at a hospital reception, the
// clerk a server puts at a desk - none of those are players; they are peds
// the server created, and to the client they are simply entries in the
// game's own ped pool.
//
// Which way one is facing is the part that matters. A server that puts a
// clerk behind a desk checks that the player is in front of him before it
// will talk, because that is what walking up to somebody means; standing at
// his shoulder, half a metre away, gets nothing at all and looks from the
// outside exactly like a player who does not know what he is doing.
//
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::game {

struct Ped {
  std::uintptr_t at_address = 0;
  Vec3  position;
  float heading = 0;      // radians, atan2 style, the direction he faces
  float away_m = 0;
  bool  is_player = false;
};

// The peds within `radius` of a point, nearest first, whatever made them.
// Reading only. `skip` is an address not to report - the character's own.
std::vector<Ped> PedsNear(const Vec3& at, float radius, std::size_t max,
                          std::uintptr_t skip = 0);

// Where to stand to be in front of somebody's face: `away` metres along the
// direction he is looking. Standing there and turning to face him is what
// walking up to a person is.
Vec3 InFrontOf(const Ped& who, float away);

std::string PedsNote();

}  // namespace gtabot::game
