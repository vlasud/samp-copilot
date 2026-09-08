#pragma once
//
// Driving the car he is sitting in, along a road route.
//
// The same shape as the walk, and for the same reason: a route is a chain of
// points, and something has to press the keys that get the thing along it.
// What is different is the vehicle. A car does not sidestep, cannot stop on
// a coin, and has to be pointed before it can be sent - so this steers
// towards the next point, gives it throttle while it is roughly pointed the
// right way, brakes when it is not, and backs out when it has wedged itself.
//
// Nothing here is written into the game. The throttle, the brake and the
// steering are the keys the player has bound to them, held through the
// system exactly as the walk holds its own.
//
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::act {

using game::Vec3;

struct DriveStatus {
  bool  driving = false;
  int   leg = 0, legs = 0;
  float to_next_m = 0;
  float remaining_m = 0;
  float speed_kmh = 0;
  float heading_error_deg = 0;
  int   times_stuck = 0;
  std::string note = "idle";
};

// Plans a road route from where the car is and starts driving it. False when
// there is no route, or he is not in a vehicle; `note` says which.
bool DriveTo(const Vec3& destination, std::string* note);

void DriveStop(const char* why);
DriveStatus DriveGet();

// Game thread, once a frame.
void DriveTick();

}  // namespace gtabot::act
