#include "actions/driver.hpp"

#include <windows.h>

#include <cmath>
#include <mutex>
#include <vector>

#include "game/bindings.hpp"
#include "game/mouse_watch.hpp"
#include "log.hpp"
#include "nav/roads.hpp"
#include "samp/input_state.hpp"
#include "samp/keys.hpp"
#include "samp/world.hpp"
#include "state/memory.hpp"
#include "ui/overlay.hpp"

namespace gtabot::act {
namespace {

// CPed. Fifty is at the wheel or in a seat; the speed is the physical body's
// own, in game units a frame, which is metres per fiftieth of a second.
constexpr std::uint32_t kPedState  = 0x530;
constexpr int kInVehicle = 50;

// How near counts as reached. A car cannot thread a needle, and a road node
// is the middle of a lane rather than a place to stop.
constexpr float kReachedNode = 12.0f;
constexpr float kReachedLast = 6.0f;
// Pointed near enough to give it throttle, and so far off that it is worth
// slowing down to come round.
constexpr float kSteerDeadZone = 0.09f;    // radians
constexpr float kThrottleWithin = 1.20f;
constexpr float kBrakeBeyond    = 2.00f;
// A corner taken at speed is a corner missed.
constexpr float kCornerSpeedKmh = 45.0f;
constexpr float kTopSpeedKmh    = 110.0f;
// Wedged: hardly moving with the throttle down.
constexpr unsigned long long kStuckAfterMs = 3500;
constexpr unsigned long long kReverseForMs = 1600;
constexpr int kGiveUpAfterStuck = 4;

std::mutex g_mutex;
bool g_driving = false;
std::vector<Vec3> g_route;
std::size_t g_leg = 0;
std::string g_note = "idle";
float g_to_next = 0, g_remaining = 0, g_speed = 0, g_error = 0;
int   g_stuck_count = 0;
unsigned long long g_moving_since = 0;
unsigned long long g_reversing_until = 0;
int g_key_accelerate = 'W', g_key_brake = 'S';
int g_key_left = 'A', g_key_right = 'D';
bool g_keys_read = false;

float Normalise(float radians) {
  while (radians > 3.14159265f)  radians -= 6.28318531f;
  while (radians < -3.14159265f) radians += 6.28318531f;
  return radians;
}

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

void ReadBindings() {
  if (g_keys_read) return;
  g_keys_read = true;
  g_key_accelerate = game::KeyForAction(game::kVehicleAccelerate, 'W');
  g_key_brake      = game::KeyForAction(game::kVehicleBrake, 'S');
  g_key_left       = game::KeyForAction(game::kVehicleSteerLeft, 'A');
  g_key_right      = game::KeyForAction(game::kVehicleSteerRight, 'D');
  LOG_INFO("driver: keys {} throttle, {} brake, {} and {} to steer",
           game::KeyName(g_key_accelerate), game::KeyName(g_key_brake),
           game::KeyName(g_key_left), game::KeyName(g_key_right));
}

// Measured, not read. A ped sitting in a car has no speed of his own - the
// car carries him - and reading his told the driver it was stuck while it
// was doing fifty. Where he was against where he is answers it without
// needing to know where the game keeps a vehicle.
float MeasureSpeed(const Vec3& here, unsigned long long now) {
  static Vec3 was{};
  static unsigned long long was_ms = 0;
  static float smoothed = 0;
  if (was_ms == 0 || now <= was_ms) {
    was = here;
    was_ms = now;
    return smoothed;
  }
  const float seconds = static_cast<float>(now - was_ms) / 1000.0f;
  if (seconds < 0.15f) return smoothed;
  const float kmh = Distance2D(was, here) / seconds * 3.6f;
  was = here;
  was_ms = now;
  smoothed = smoothed * 0.5f + kmh * 0.5f;
  return smoothed;
}

bool InVehicle(std::uintptr_t ped) {
  int state = -1;
  return ped != 0 && asi::mem::Read<int>(ped + kPedState, &state) &&
         state == kInVehicle;
}

void StopLocked(std::string why) {
  g_driving = false;
  g_route.clear();
  g_leg = 0;
  g_note = std::move(why);
  samp::KeysReleaseAll();
}

// Whether the keys may go out at all, on the same terms as the walk's.
bool MayDrive() {
  HWND window = game::GameWindow();
  if (window == nullptr || GetForegroundWindow() != window) return false;
  if (asi::Overlay::MenuOpen()) return false;
  return !samp::InputLegitimatelyOff(nullptr);
}

}  // namespace

bool DriveTo(const Vec3& destination, std::string* note) {
  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) {
    *note = "the local player is not readable";
    return false;
  }
  if (!InVehicle(self.game_ped)) {
    *note = "he is not in a vehicle - walk him to one and press use_vehicle";
    return false;
  }
  const Vec3 here{self.x, self.y, self.z};
  const nav::Road road = nav::RoadRoute(here, destination);
  if (!road.ok || road.points.size() < 2) {
    *note = "no road route: " + road.note;
    return false;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  ReadBindings();
  g_route = road.points;
  g_leg = 0;
  g_driving = true;
  g_stuck_count = 0;
  g_moving_since = GetTickCount64();
  g_reversing_until = 0;
  g_note = "driving";
  LOG_INFO("drive: {} - {} points, first at ({:.0f}, {:.0f})", road.note,
           road.points.size(), road.points.front().x, road.points.front().y);
  *note = road.note;
  return true;
}

void DriveStop(const char* why) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_driving) LOG_INFO("drive: {}", why);
  StopLocked(why);
}

DriveStatus DriveGet() {
  std::lock_guard<std::mutex> lock(g_mutex);
  DriveStatus status;
  status.driving = g_driving;
  status.leg = static_cast<int>(g_leg);
  status.legs = static_cast<int>(g_route.size());
  status.to_next_m = g_to_next;
  status.remaining_m = g_remaining;
  status.speed_kmh = g_speed;
  status.heading_error_deg = g_error * 57.2957795f;
  status.times_stuck = g_stuck_count;
  status.note = g_note;
  return status;
}

void DriveTick() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_driving) return;

  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) {
    StopLocked("stopped - the character cannot be read");
    return;
  }
  if (!InVehicle(self.game_ped)) {
    StopLocked("stopped - he is no longer in a vehicle");
    LOG_INFO("drive: {}", g_note);
    return;
  }
  if (!MayDrive()) {
    samp::KeysReleaseAll();
    return;
  }

  const unsigned long long now = GetTickCount64();
  const Vec3 here{self.x, self.y, self.z};
  g_speed = MeasureSpeed(here, now);

  // Points passed. A node is reached generously: a car that insists on the
  // exact middle of a junction spends the corner reversing into it.
  while (g_leg < g_route.size()) {
    const bool last = g_leg + 1 == g_route.size();
    const float distance = Distance2D(here, g_route[g_leg]);
    bool done = distance <= (last ? kReachedLast : kReachedNode);
    if (!done && !last && Distance2D(here, g_route[g_leg + 1]) < distance)
      done = true;
    if (!done) break;
    ++g_leg;
  }
  if (g_leg >= g_route.size()) {
    StopLocked("arrived");
    LOG_INFO("drive: arrived");
    return;
  }

  const Vec3& target = g_route[g_leg];
  g_to_next = Distance2D(here, target);
  g_remaining = g_to_next;
  for (std::size_t i = g_leg + 1; i < g_route.size(); ++i)
    g_remaining += Distance2D(g_route[i - 1], g_route[i]);

  const float wanted = std::atan2(target.y - here.y, target.x - here.x);
  g_error = Normalise(wanted - self.heading);

  // Wedged against something: back out, straighten, try again.
  if (g_speed > 6.0f) g_moving_since = now;
  if (now < g_reversing_until) {
    samp::KeysHold({g_key_brake, g_error > 0 ? g_key_right : g_key_left});
    return;
  }
  if (now - g_moving_since > kStuckAfterMs) {
    ++g_stuck_count;
    g_moving_since = now;
    if (g_stuck_count > kGiveUpAfterStuck) {
      StopLocked("stuck - the car will not come free");
      LOG_WARN("drive: {} at ({:.0f}, {:.0f})", g_note, here.x, here.y);
      return;
    }
    g_reversing_until = now + kReverseForMs;
    LOG_INFO("drive: not moving - backing out (attempt {})", g_stuck_count);
    return;
  }

  std::vector<int> hold;
  const float away = std::fabs(g_error);
  if (away > kSteerDeadZone) hold.push_back(g_error > 0 ? g_key_left : g_key_right);

  // Throttle while it is pointed somewhere near the right way and is not
  // already going faster than the next corner allows.
  const float corner = g_to_next < 25.0f || away > 0.6f ? kCornerSpeedKmh
                                                        : kTopSpeedKmh;
  if (away > kBrakeBeyond || g_speed > corner + 15.0f)
    hold.push_back(g_key_brake);
  else if (away < kThrottleWithin && g_speed < corner)
    hold.push_back(g_key_accelerate);

  samp::KeysHold(hold);
}

}  // namespace gtabot::act
