#pragma once
//
// What is free all the way round him, right now.
//
// The route says where to go; it does not say that somebody has just stepped
// into the doorway, or that the corner he is cutting has a bin against it.
// Contact steering handles those - after he has walked into them. Handling
// them before is what stops him touching things at all, which is the
// difference between a person walking down a corridor and a machine bouncing
// off its walls.
//
// So: a fan of short lines all the way round, every tenth of a second, and
// the one nearest to where he wants to go that is open is where he goes. The
// lines are answered from the collision pools - buildings, dummies, the
// server's objects - and from the ped pool for whoever is standing about,
// never by calling into the game.
//
#include <string>

#include "game/world_query.hpp"

namespace gtabot::nav {

using game::Vec3;

struct Ring {
  bool  ok = false;
  int   spokes = 0;
  float free_ahead_m = 0;    // along the wanted heading
  float steer = 0;           // the heading to walk: wanted, or the nearest open one
  bool  turned = false;      // whether it had to be moved off the wanted one
  float turned_by_deg = 0;
};

// `wanted` is the heading he means to walk; `reach` how far ahead to care
// about. Game thread, cheap enough for every frame.
Ring LookRound(const Vec3& here, float wanted, float reach);

std::string RingNote();

}  // namespace gtabot::nav
