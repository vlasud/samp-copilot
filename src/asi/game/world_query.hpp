#pragma once
//
// Asks the game about its own world: what is under a point, whether a line
// between two points passes through anything, where a point lands on the
// screen.
//
// These are calls into gta_sa.exe, not reads of it. They are the functions
// the game's own pedestrians use to decide where they can walk, which is the
// whole reason to use them rather than reason about geometry ourselves: the
// collision the game consults is the collision that will actually stop a
// character. Two consequences follow. They may only run on the game thread,
// because that is where the world is consistent. And they only know about
// what is streamed in - the few hundred metres around the player - so a
// question about somewhere far away is answered "no ground", which is not a
// verdict about the place, only about how far the game can see.
//
// Nothing here runs until the executable is recognised (game/exe.hpp) and
// the ground function has been checked against the one fact we can verify
// without it: the ground under the player is where the player is standing.
//
#include <cstdint>

namespace gtabot::game {

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

// Whether the calls below can be trusted. Established once by SelfCheck()
// and never assumed.
bool CallsTrusted();

// Runs the check that decides CallsTrusted(): with the local player's
// position, the ground the game reports under him must be about a ped's
// height below him. Game thread only. Returns the verdict and says why.
bool SelfCheck(const Vec3& player_position, const char** why);

// The ground below (x, y, z), searching downwards from z. False when the
// game finds none - which is also the answer for anywhere not streamed in.
bool GroundBelow(const Vec3& at, float* ground_z);

// Whether nothing solid lies between a and b: buildings, objects, vehicles
// and the dummies fences are made of. Peds are deliberately left out - they
// move, and a route is not blocked by someone standing in it right now.
bool LineClear(const Vec3& a, const Vec3& b);

// Where a world point appears on screen, in pixels. False when it is behind
// the camera.
bool ToScreen(const Vec3& world, float* sx, float* sy);

}  // namespace gtabot::game
