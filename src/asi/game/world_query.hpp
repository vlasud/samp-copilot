#pragma once
//
// Asks the game about its own world: what is under a point, whether a line
// between two points passes through anything, where a point lands on the
// screen.
//
// These were calls into gta_sa.exe. SA-MP's client protection answers a
// call into the game's world functions from outside with the keyboard
// taken away, while a module that only reads is left alone - so since the
// evening of 2026-09-08 they are reads: the same sectors, entities and
// collision models the game's functions walk, walked here (game/collision).
// Two things remain as they were. This runs only on the game thread,
// because that is where the world is consistent. And it only knows about
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

// The master switch. The whole subsystem is off until this is set, and no
// function here calls into the game while it is off. Default off, deliberately:
// a machine that had to be rebooted is a high enough price that movement does
// not turn itself on.
void SetEnabled(bool on);
bool Enabled();

// The staged arming that found the fault is gone. It brought the three calls
// in on a twenty-five second clock so that whichever stage the input died in
// would name the call. It did its job; leaving it in only meant nothing could
// be drawn for the first fifty seconds after arming. What earned a permanent
// place instead are the self-checks below, which ask whether a call works
// rather than when it is allowed.

// How many calls into the game were made in the last second, and the ceiling
// on them. The ceiling covers the calls that walk the game's collision; the
// projection is arithmetic on the camera and is not counted, or a fan drawn
// at ninety frames a second would spend the whole allowance on itself.
int CallsInLastSecond();
int CallsPerSecondCeiling();
// How many more calls this second will still be answered by the game. Past
// the ceiling every query says "no ground" and "blocked", which a planner
// must not mistake for the world; it waits instead.
int CallSlotsLeft();

// How many of each kind have been made since arming, so a report of the input
// going away can name what had been called by then.
int GroundCalls();
int LineOfSightCalls();
int ScreenCalls();

// Whether the calls below can be trusted: enabled, and SelfCheck() has passed.
bool CallsTrusted();

// Runs the check that decides CallsTrusted(): with the local player's
// position, the ground the game reports under him must be about a ped's
// height below him. Game thread only. Returns the verdict and says why.
bool SelfCheck(const Vec3& player_position, const char** why);

// The same treatment for the line-of-sight call, which never had it.
//
// It is the one call no successful session ever made and every failing one
// made in bulk, and it answers "no headroom" for a man standing upright -
// which is not an answer a working line-of-sight test gives. So it has to
// earn its place against two facts about where the player is standing: the
// space between his knees and his head is clear, and a line from above him to
// below the ground is not. Until it passes, it is never called.
bool SelfCheckLineOfSight(const Vec3& player_position, const char** why);
bool LineOfSightTrusted();

// Whether a line-of-sight answer can be had at all right now: verified, armed
// and past the stage that brings it in. Callers need this to tell "the way is
// blocked" from "nobody asked" - reporting the second as the first is how
// every plan in the first twenty-five seconds came back "no headroom".
bool LineOfSightAvailable();

// The ground below (x, y, z), searching downwards from z. False when the
// game finds none - which is also the answer for anywhere not streamed in.
bool GroundBelow(const Vec3& at, float* ground_z, bool include_objects = true);

// The water surface at (x, y), if there is water there at all. The ground
// call does not know about water - it finds the lake bed and calls it
// ground - so every place he is sent is asked this as well.
bool WaterLevel(const Vec3& at, float* level);

// Whether nothing solid lies between a and b: buildings, objects, vehicles
// and the dummies fences are made of. Peds are deliberately left out - they
// move, and a route is not blocked by someone standing in it right now.
bool LineClear(const Vec3& a, const Vec3& b, bool include_vehicles = true);

// Where a world point appears on screen, in pixels. False when it is behind
// the camera.
bool ToScreen(const Vec3& world, float* sx, float* sy);

// Both ends of a line at once, cut where it passes the camera rather than
// dropped when one end is behind it. False only when there is nothing of
// the line in front of the camera at all.
bool ToScreenLine(const Vec3& from, const Vec3& to, float* ax, float* ay,
                  float* bx, float* by);

// Whether the game itself has switched the player's controls off. This is
// the flag SA-MP sets while a dialog is up, and the one the server sets by
// freezing a player. A character that will not move while this is set is not
// an input problem of ours. Read-only; false when the build is unknown.
bool ControlsDisabled(bool* disabled);

// Which way the camera looks, as an angle in radians measured the way atan2
// measures one. On foot the walk stick is camera-relative - pushing forward
// means "away from the camera", not "north" - so nothing can be steered
// without this. False when it cannot be read or does not check out.
//
// CCamera derives from CPlaceable, so its matrix is where an entity's is, and
// the check is the one that catches a wrong address: the forward row of a
// real matrix is a unit vector.
bool CameraHeading(float* radians);

// The camera's own orientation angle, the one the game subtracts when it
// turns the walk stick into a direction: in CTaskSimplePlayerOnFoot's
// PlayerControlZelda the heading is GetRadianAngleBetweenPoints(0, 0,
// -stickX, stickY) - TheCamera.m_fOrientation. Read, never written - the
// camera stays the player's. False when it cannot be read or is not a
// plausible angle.
bool CameraOrientation(float* radians);

}  // namespace gtabot::game
