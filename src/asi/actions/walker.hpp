#pragma once
//
// Walks the character along a route, by working his controller.
//
// Nothing here moves him. It presses the stick, and the game moves him: the
// same physics, the same animations, the same speed, and therefore the same
// position updates going to the server as a person walking. That is the whole
// design constraint. Writing a position would be a teleport, which is both
// visible to a server and not what walking means; pressing the stick is
// indistinguishable from a hand on the keyboard because it *is* the path a
// hand on the keyboard takes.
//
// It writes at exactly one moment: immediately after CPad::UpdatePads, which
// is where the game has just filled the pad from the real keyboard and has not
// yet read it. Anywhere later - our frame hooks, for instance - and the next
// call to UpdatePads overwrites it before the character is processed, so the
// stick is pressed and nothing happens.
//
// On foot the stick is camera-relative: forward means away from the camera,
// not north. So a heading has to be turned into a stick position through the
// camera's own orientation, and the sign conventions of that transform are
// not guessed - the walker measures where the character actually went against
// where it meant to send him, and corrects itself once if they disagree.
//
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::act {

using game::Vec3;

struct Status {
  bool        walking = false;
  int         leg     = 0;
  int         legs    = 0;
  float       to_next_m   = 0;
  float       remaining_m = 0;
  // What the last walk ended with: arrived, stuck, given up, stopped.
  std::string note;
  // Whether the camera-relative transform had to correct its own sign, and
  // how far off the character's actual heading is from the intended one.
  bool        corrected = false;
  float       error_deg = 0;
  // How many times he has had to step round something on this walk.
  int         sidesteps = 0;
};

// Hooks CPad::UpdatePads. MinHook must already be initialised, and the
// executable must be the build these addresses are for.
bool Install();
void Uninstall();

// Start walking. The route is waypoints in order, the character's own
// position included or not - whichever, he walks to each in turn.
void WalkTo(std::vector<Vec3> route);

// Let go of the stick. Safe to call when not walking.
void Stop(const char* why);

Status Get();

}  // namespace gtabot::act
