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
// The route is a suggestion about the world as it was when it was planned.
// Between planning and walking a car parks across the pavement, a gate
// closes, a crowd forms. So the walker looks where it is going: a few short
// lines of sight ahead - whiskers - every tenth of a second, and it leans
// away from whatever they touch before he runs into it. Only when they all
// touch something does it stop and hand the problem back to the journey,
// which plans again around what was found.
//
// He runs. Sprint is held whenever the way ahead is clear and there is far
// enough to go, and while sprinting he jumps - a sprint jump carries more
// speed than the run it starts from, which is why every player on the server
// crosses a city that way.
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
  // What the last walk ended with: arrived, stuck, blocked, given up, stopped.
  std::string note;
  // Whether the camera-relative transform had to correct its own sign, and
  // how far off the character's actual heading is from the intended one.
  bool        corrected = false;
  float       error_deg = 0;
  // How many times he has had to step round something on this walk.
  int         sidesteps = 0;
  // What the whiskers made him do: how far he is leaning off the line, in
  // degrees, and whether every whisker was blocked last time.
  float       steer_deg = 0;
  bool        wall      = false;
  bool        sprinting = false;
  int         jumps     = 0;
};

// Hooks CPad::UpdatePads. MinHook must already be initialised, and the
// executable must be the build these addresses are for.
bool Install();
void Uninstall();

// Game thread, once per frame from the frame hook, after the journey has
// ticked: decides the stick and writes it into the pad's keyboard temp
// state for the next CPad::UpdatePads to pick up.
void PadFrame();

// Drops the legs before this one (an index into the route given to WalkTo)
// and heads straight for it. False when there is no walk, or it is not
// ahead of the current leg.
bool CutTo(std::size_t leg);

// Key events sent through the system so far, for the frame record.
unsigned long long KeyEventsSent();

// The key experiment: forward and sprint held through the system every
// frame, with no walk and no call into the game, until told to stop.
void HoldTestKeys(bool hold);

// Start walking. The route is waypoints in order, the character's own
// position included or not - whichever, he walks to each in turn.
void WalkTo(std::vector<Vec3> route);

// Let go of the stick. Safe to call when not walking.
void Stop(const char* why);

// Whether to run rather than walk, and whether to jump while running. Both
// on by default; the panel can switch either off to compare.
void SetSprint(bool on);
void SetBunnyHop(bool on);
bool Sprint();
bool BunnyHop();

Status Get();

}  // namespace gtabot::act
