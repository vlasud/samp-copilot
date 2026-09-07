#include "actions/walker.hpp"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "game/exe.hpp"
#include "hooks/windowmode.hpp"
#include "log.hpp"
#include "samp/world.hpp"
#include "state/memory.hpp"

namespace gtabot::act {
namespace {

// CPad::UpdatePads fills the pad from the real keyboard and mouse. Writing
// immediately after it is the only moment the stick sticks.
constexpr std::uint32_t kUpdatePads = 0x541DD0;
constexpr std::uint32_t kPads       = 0xB73458;
// CPad is 0x134 bytes; NewState is the first member and its sticks are the
// first two shorts of it.
constexpr std::uint32_t kNewStateLeftStickX = 0x00;
constexpr std::uint32_t kNewStateLeftStickY = 0x02;

// Full deflection. The game clamps to 128 either way.
constexpr short kFullStick = 127;

// How close counts as arrived. A person does not stop on a coin, and the
// route's own legs are metres long.
constexpr float kArriveNext = 1.6f;
constexpr float kArriveLast = 1.0f;
// Giving up: no meaningful progress toward the next point for this long.
constexpr unsigned long long kStuckMs = 2500;
constexpr float kProgress = 0.4f;
// The whole walk, so a route that cannot be finished does not press the stick
// forever.
constexpr unsigned long long kWalkLimitMs = 120000;

// Measuring the transform rather than trusting it. After this far travelled,
// where he actually went is compared with where he was sent.
constexpr float kCalibrateAfter = 1.5f;

using UpdatePadsFn = void(__cdecl*)();
UpdatePadsFn g_original_update = nullptr;
void*        g_hook_target = nullptr;
std::atomic<bool> g_installed{false};

std::mutex        g_mutex;
std::vector<Vec3> g_route;
std::size_t       g_leg = 0;
bool              g_walking = false;
std::string       g_note = "idle";
unsigned long long g_started_ms = 0;
unsigned long long g_progress_ms = 0;
float             g_best_distance = 0;
float             g_to_next = 0;
float             g_remaining = 0;

// The camera-relative transform's one uncertain sign, and the evidence for
// it. Sideways is the axis a convention can be backwards about; forward is
// not, because walking away from a target is obvious in the first metre.
bool  g_flip_sideways = false;
bool  g_calibrated = false;
bool  g_corrected = false;
float g_error_deg = 0;
Vec3  g_calibrate_from;
float g_calibrate_heading = 0;

float Normalise(float radians) {
  while (radians > 3.14159265f)  radians -= 6.28318531f;
  while (radians < -3.14159265f) radians += 6.28318531f;
  return radians;
}

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

void ClearStick() {
  const std::uintptr_t pad = game::At(kPads);
  if (pad == 0) return;
  auto* x = reinterpret_cast<short*>(pad + kNewStateLeftStickX);
  auto* y = reinterpret_cast<short*>(pad + kNewStateLeftStickY);
  if (!asi::mem::IsReadable(pad, 4)) return;
  *x = 0;
  *y = 0;
}

void StopLocked(const char* why) {
  g_walking = false;
  g_route.clear();
  g_leg = 0;
  g_note = why;
  ClearStick();
}

// Everything the walk decides, run from inside the pad hook. Returns the
// stick to press, or false to press nothing.
bool DecideStick(short* out_x, short* out_y) {
  if (!g_walking) return false;

  const unsigned long long now = GetTickCount64();
  if (now - g_started_ms > kWalkLimitMs) {
    StopLocked("given up - the walk ran out of time");
    LOG_WARN("walk: {}", g_note);
    return false;
  }

  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) {
    StopLocked("stopped - the character cannot be read");
    return false;
  }
  const Vec3 here{self.x, self.y, self.z};

  // Arrived at this leg?
  while (g_leg < g_route.size()) {
    const bool last = g_leg + 1 == g_route.size();
    const float d = Distance2D(here, g_route[g_leg]);
    if (d > (last ? kArriveLast : kArriveNext)) break;
    ++g_leg;
    g_progress_ms = now;
    g_best_distance = 0;
  }
  if (g_leg >= g_route.size()) {
    StopLocked("arrived");
    LOG_INFO("walk: arrived");
    return false;
  }

  const Vec3& target = g_route[g_leg];
  const float distance = Distance2D(here, target);
  g_to_next = distance;
  g_remaining = distance;
  for (std::size_t i = g_leg + 1; i < g_route.size(); ++i)
    g_remaining += Distance2D(g_route[i - 1], g_route[i]);

  // Progress, or the want of it.
  if (g_best_distance == 0 || distance < g_best_distance - kProgress) {
    g_best_distance = distance;
    g_progress_ms = now;
  } else if (now - g_progress_ms > kStuckMs) {
    StopLocked("stuck - no progress toward the next point");
    LOG_WARN("walk: stuck {:.1f} m short of leg {} of {}", distance,
             static_cast<int>(g_leg) + 1, static_cast<int>(g_route.size()));
    return false;
  }

  float camera = 0;
  if (!game::CameraHeading(&camera)) {
    StopLocked("stopped - the camera cannot be read, so nothing can be steered");
    LOG_WARN("walk: {}", g_note);
    return false;
  }

  const float wanted = std::atan2(target.y - here.y, target.x - here.x);

  // Where he actually went against where he was sent. Done once, after enough
  // ground has been covered for the answer to mean something.
  if (!g_calibrated && Distance2D(g_calibrate_from, here) >= kCalibrateAfter) {
    const float went = std::atan2(here.y - g_calibrate_from.y,
                                  here.x - g_calibrate_from.x);
    const float error = Normalise(went - g_calibrate_heading);
    g_error_deg = error * 57.2957795f;
    g_calibrated = true;
    if (std::fabs(error) > 1.0472f) {   // more than sixty degrees out
      g_flip_sideways = !g_flip_sideways;
      g_corrected = true;
      LOG_INFO("walk: he went {:.0f} degrees off where he was sent, so the "
               "sideways axis is the other way round - corrected", g_error_deg);
    } else {
      LOG_INFO("walk: heading agrees to within {:.0f} degrees", g_error_deg);
    }
  }
  if (!g_calibrated) {
    g_calibrate_from = here;
    g_calibrate_heading = wanted;
  }

  const float relative = Normalise(wanted - camera);
  const float sideways = std::sin(relative) * (g_flip_sideways ? 1.0f : -1.0f);
  const float forward  = std::cos(relative);
  *out_x = static_cast<short>(sideways * kFullStick);
  *out_y = static_cast<short>(-forward * kFullStick);
  return true;
}

void __cdecl HookedUpdatePads() {
  g_original_update();

  // The pad has just been filled from the real keyboard. Ours goes on top,
  // and only while a walk is running - the rest of the time the player's own
  // input passes through untouched.
  short x = 0, y = 0;
  bool press = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    press = DecideStick(&x, &y);
  }
  if (!press) return;

  const std::uintptr_t pad = game::At(kPads);
  if (pad == 0 || !asi::mem::IsReadable(pad, 4)) return;
  *reinterpret_cast<short*>(pad + kNewStateLeftStickX) = x;
  *reinterpret_cast<short*>(pad + kNewStateLeftStickY) = y;
}

}  // namespace

bool Install() {
  if (g_installed.load()) return true;
  // The only place this module writes into gta_sa.exe's own code, and that
  // executable is protected - instructions relocated into stubs, obfuscation
  // around them. Worth being able to switch off without a rebuild.
  if (!asi::WindowMode::WalkerAllowed()) {
    static bool said = false;
    if (!said) {
      said = true;
      LOG_INFO("walker: bot.cfg says walker=off - the game's code is left "
               "alone and the character cannot be walked");
    }
    return false;
  }
  auto* target = reinterpret_cast<void*>(game::At(kUpdatePads));
  if (target == nullptr) {
    LOG_WARN("walker: not the build CPad::UpdatePads is known for - the "
             "character cannot be walked");
    return false;
  }
  if (MH_CreateHook(target, &HookedUpdatePads,
                    reinterpret_cast<void**>(&g_original_update)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    LOG_ERROR("walker: could not hook CPad::UpdatePads");
    return false;
  }
  g_hook_target = target;
  g_installed.store(true);
  LOG_INFO("walker installed on CPad::UpdatePads at gta_sa.exe+0x{:X}",
           kUpdatePads - 0x400000);
  return true;
}

void Uninstall() {
  if (!g_installed.load()) return;
  Stop("stopped - shutting down");
  if (g_hook_target) MH_RemoveHook(g_hook_target);
  g_installed.store(false);
}

void WalkTo(std::vector<Vec3> route) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (route.empty()) {
    StopLocked("nothing to walk to");
    return;
  }
  g_route = std::move(route);
  g_leg = 0;
  g_walking = true;
  g_note = "walking";
  g_started_ms = GetTickCount64();
  g_progress_ms = g_started_ms;
  g_best_distance = 0;
  g_calibrated = false;
  g_corrected = false;
  g_error_deg = 0;
  const samp::LocalPed self = samp::ReadLocalPed();
  g_calibrate_from = self.valid ? Vec3{self.x, self.y, self.z} : g_route.front();
  LOG_INFO("walk: {} legs, first at ({:.1f}, {:.1f})", g_route.size(),
           g_route.front().x, g_route.front().y);
}

void Stop(const char* why) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_walking) LOG_INFO("walk: {}", why);
  StopLocked(why);
}

Status Get() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Status status;
  status.walking     = g_walking;
  status.leg         = static_cast<int>(g_leg);
  status.legs        = static_cast<int>(g_route.size());
  status.to_next_m   = g_to_next;
  status.remaining_m = g_remaining;
  status.note        = g_note;
  status.corrected   = g_corrected;
  status.error_deg   = g_error_deg;
  return status;
}

}  // namespace gtabot::act
