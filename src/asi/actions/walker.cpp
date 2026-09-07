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

// Long enough to push straight forward and see which way that turned out to
// be, and short enough that a walk does not visibly start in the wrong
// direction.
constexpr unsigned long long kBootstrapMs = 450;

// Meeting something in the way. A person does not stop dead at a bin and
// abandon the errand; he steps round it and carries on, and tries the other
// side if that does not work. Only after several of those is it really a wall.
constexpr int   kMaxSidesteps    = 4;
constexpr float kSidestepMetres  = 2.8f;
constexpr unsigned long long kSidestepMs = 1400;

// The stick is eased rather than snapped. Full deflection appearing in one
// frame is what makes a character look driven rather than walked, and the
// game's own turning is smoothed anyway - fighting it just wastes distance.
constexpr float kStickEase = 0.28f;
// Slowing for the last stride reads as arriving somewhere rather than
// colliding with it, and stops him sailing past a tight waypoint.
constexpr float kEaseInFrom = 3.0f;
constexpr float kSlowest    = 0.55f;

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

// The measured transform from stick to world, and the one thing measuring it
// cannot settle on its own: whether the sideways axis runs the other way. A
// mirrored axis makes every correction push him further out, so it shows up
// as an error that will not come down, and that is what is watched for.
float g_offset = 0;
bool  g_offset_seen = false;
float g_last_emit = 0;
float g_hand = 1.0f;
unsigned long long g_bootstrap_until = 0;
unsigned long long g_wrong_since = 0;
bool  g_corrected = false;
float g_error_deg = 0;

// Stepping round something, and the eased stick.
int   g_sidesteps = 0;
bool  g_sidestep_left = true;
unsigned long long g_sidestep_until = 0;
Vec3  g_sidestep_target;
float g_stick_x = 0, g_stick_y = 0;

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
  g_sidestep_until = 0;
  g_stick_x = 0;
  g_stick_y = 0;
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

  // Arrived at this leg - or past it.
  //
  // Insisting on reaching each point exactly is what makes a character walk
  // like a machine: he heads for a corner, touches it, then turns on the spot
  // for the next one. A person rounds the corner. So a waypoint also counts
  // as done once the one after it is nearer than it is, which is what having
  // passed something means, and the turn happens while still moving.
  while (g_leg < g_route.size()) {
    const bool last = g_leg + 1 == g_route.size();
    const float d = Distance2D(here, g_route[g_leg]);
    bool done = d <= (last ? kArriveLast : kArriveNext);
    if (!done && !last && Distance2D(here, g_route[g_leg + 1]) < d) done = true;
    if (!done) break;
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
  } else if (now - g_progress_ms > kStuckMs && now > g_sidestep_until) {
    if (g_sidesteps >= kMaxSidesteps) {
      StopLocked("stuck - stepped around four times and still no way through");
      LOG_WARN("walk: {} ({:.1f} m short of leg {} of {})", g_note, distance,
               static_cast<int>(g_leg) + 1, static_cast<int>(g_route.size()));
      return false;
    }
    // Out to one side of the way ahead, then carry on. Sides alternate, so a
    // corner that defeats one direction gets the other tried next.
    ++g_sidesteps;
    g_sidestep_left = !g_sidestep_left;
    const float ahead = std::atan2(target.y - here.y, target.x - here.x);
    const float side  = ahead + (g_sidestep_left ? 1.5708f : -1.5708f);
    g_sidestep_target = Vec3{here.x + std::cos(side) * kSidestepMetres,
                             here.y + std::sin(side) * kSidestepMetres, here.z};
    g_sidestep_until = now + kSidestepMs;
    g_progress_ms = now;
    g_best_distance = 0;
    LOG_INFO("walk: something in the way, stepping {} round it (attempt {})",
             g_sidestep_left ? "left" : "right", g_sidesteps);
  }

  // Where he is going, and where he is going to be sent.
  const bool stepping = now < g_sidestep_until;
  const Vec3& aim = stepping ? g_sidestep_target : target;
  const float wanted = std::atan2(aim.y - here.y, aim.x - here.x);

  // The stick is camera-relative, and the camera is the player's business -
  // he turns it when he likes and it is wanted for aiming later. So the
  // transform is not read out of the camera; it is measured off the
  // character. He turns to face wherever the stick sends him, and his facing
  // is already being read, so the offset between the two is observable and
  // self-correcting - it follows the camera around without ever asking it,
  // and a player spinning the view mid-walk is just another correction.
  //
  // The first fraction of a second of a walk is spent pushing straight
  // forward and watching where he ends up pointing, which is the offset
  // outright.
  const bool bootstrapping = now < g_bootstrap_until;
  const float emit = bootstrapping ? 0.0f : Normalise(wanted - g_offset);

  // What the last press implied, folded in slowly. Slowly because he takes a
  // few frames to come round, and a fast estimate would chase its own tail.
  if (g_offset_seen) {
    const float implied = Normalise(self.heading - g_last_emit);
    const float step = Normalise(implied - g_offset);
    g_offset = Normalise(g_offset + step * (bootstrapping ? 0.35f : 0.08f));
  }
  g_last_emit  = emit;
  g_offset_seen = true;

  // How far his facing is from where he is meant to be going. Kept for the
  // panel, and used to notice the one thing measurement cannot fix by
  // itself: a sideways axis that is the other way round makes every
  // correction push him further out, so the error never comes down.
  const float heading_error = Normalise(wanted - self.heading);
  g_error_deg = heading_error * 57.2957795f;
  if (!bootstrapping && std::fabs(heading_error) > 1.7453f) {
    if (g_wrong_since == 0) g_wrong_since = now;
    else if (now - g_wrong_since > 2000) {
      g_hand = -g_hand;
      g_corrected = true;
      g_wrong_since = 0;
      g_offset_seen = false;
      g_bootstrap_until = now + kBootstrapMs;
      LOG_INFO("walk: still pointing {:.0f} degrees away after correcting, so "
               "the sideways axis is the other way round - flipped",
               g_error_deg);
    }
  } else {
    g_wrong_since = 0;
  }

  // Ease off over the last few metres of the last leg.
  float pace = 1.0f;
  const bool final_leg = g_leg + 1 == g_route.size();
  if (final_leg && !stepping && distance < kEaseInFrom)
    pace = kSlowest + (1.0f - kSlowest) * (distance / kEaseInFrom);

  const float want_x = std::sin(emit) * g_hand * kFullStick * pace;
  const float want_y = -std::cos(emit) * kFullStick * pace;
  g_stick_x += (want_x - g_stick_x) * kStickEase;
  g_stick_y += (want_y - g_stick_y) * kStickEase;
  *out_x = static_cast<short>(g_stick_x);
  *out_y = static_cast<short>(g_stick_y);
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
  g_offset_seen = false;
  g_bootstrap_until = GetTickCount64() + kBootstrapMs;
  g_wrong_since = 0;
  g_corrected = false;
  g_error_deg = 0;
  g_sidesteps = 0;
  g_sidestep_until = 0;
  g_stick_x = 0;
  g_stick_y = 0;
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
  status.sidesteps   = g_sidesteps;
  return status;
}

}  // namespace gtabot::act
