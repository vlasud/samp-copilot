#include "actions/travel.hpp"

#include <windows.h>

#include <cmath>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "actions/walker.hpp"
#include "game/paths.hpp"
#include "log.hpp"
#include "nav/planner.hpp"
#include "samp/world.hpp"

namespace gtabot::act {
namespace {

// Close enough to have got there.
constexpr float kArrived = 2.5f;
// A staging point has to be worth walking to, or the journey stalls on the
// spot replanning to where it already is.
constexpr float kMinStagingStep = 12.0f;
// How far to look for one. Beyond the loaded areas there is nothing to find.
constexpr float kStagingSearch = 400.0f;
constexpr std::size_t kStagingNodes = 600;
// Attempts in a row that end no nearer than they started.
constexpr int kMaxFailures = 4;
// Planning is not free, and the walker needs a moment to actually set off.
constexpr unsigned long long kReplanGapMs = 700;

std::mutex  g_mutex;
bool        g_travelling = false;
Vec3        g_destination;
int         g_replans  = 0;
int         g_failures = 0;
bool        g_reaching = false;
std::string g_note = "idle";
float       g_best_straight = 0;
unsigned long long g_next_plan_ms = 0;

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

void StopLocked(std::string why) {
  g_travelling = false;
  g_note = std::move(why);
}

// The loaded ped node that gets furthest toward the destination. This is what
// makes crossing a city possible at all: the far side has no ground under it
// yet, so the journey is made of the longest legs the loaded world can
// currently justify, replanned as more of it appears.
bool StagingPoint(const Vec3& here, const Vec3& destination, Vec3* out) {
  const std::vector<game::PathNode> nodes =
      game::PedNodesNear(here, kStagingSearch, kStagingNodes);
  const game::PathNode* best = nullptr;
  float best_distance = Distance2D(here, destination);
  for (const game::PathNode& node : nodes) {
    if (Distance2D(here, node.pos) < kMinStagingStep) continue;
    const float toward = Distance2D(node.pos, destination);
    if (toward >= best_distance) continue;
    best_distance = toward;
    best = &node;
  }
  if (best == nullptr) return false;
  *out = Vec3{best->pos.x, best->pos.y, best->pos.z + 1.0f};
  return true;
}

// One attempt: plan to the destination, or failing that to the best staging
// point, and set the walker going.
bool PlanAndWalk(const Vec3& here) {
  nav::Plan plan = nav::PlanPath(here, g_destination);
  g_reaching = false;

  if (!plan.ok || plan.waypoints.size() < 2) {
    Vec3 staging;
    if (!StagingPoint(here, g_destination, &staging)) {
      g_note = "no route and nowhere nearer to head for: " + plan.note;
      return false;
    }
    plan = nav::PlanPath(here, staging);
    g_reaching = true;
    if (!plan.ok || plan.waypoints.size() < 2) {
      g_note = "the way to the nearest staging point is blocked too: " +
               plan.note;
      return false;
    }
  }

  nav::SetDebugPlan(g_destination, plan);
  WalkTo(std::vector<Vec3>(plan.waypoints.begin() + 1, plan.waypoints.end()));
  ++g_replans;
  g_note = g_reaching ? "walking toward the far side" : "walking to the target";
  return true;
}

}  // namespace

void TravelTo(const Vec3& destination) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Stop("replaced by a journey");
  g_destination = destination;
  g_travelling  = true;
  g_replans     = 0;
  g_failures    = 0;
  g_reaching    = false;
  g_best_straight = 0;
  g_next_plan_ms  = 0;
  g_note = "starting";
  LOG_INFO("travel: to ({:.1f}, {:.1f})", destination.x, destination.y);
}

void CancelTravel(const char* why) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_travelling) LOG_INFO("travel: {}", why);
  StopLocked(why);
  Stop(why);
}

void TravelTick() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_travelling) return;

  // While the walker is on its feet there is nothing to decide.
  if (Get().walking) return;

  const unsigned long long now = GetTickCount64();
  if (now < g_next_plan_ms) return;
  g_next_plan_ms = now + kReplanGapMs;

  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) {
    StopLocked("stopped - the character cannot be read");
    LOG_WARN("travel: {}", g_note);
    return;
  }
  const Vec3 here{self.x, self.y, self.z};
  const float straight = Distance2D(here, g_destination);

  if (straight <= kArrived) {
    StopLocked("arrived");
    LOG_INFO("travel: arrived, {} legs planned along the way", g_replans);
    return;
  }

  // Did the last attempt get us anywhere? Measured against the best we have
  // managed, so shuffling back and forth counts as the failure it is.
  if (g_best_straight == 0 || straight < g_best_straight - 1.0f) {
    g_best_straight = straight;
    g_failures = 0;
  } else if (++g_failures >= kMaxFailures) {
    StopLocked("gave up - " + std::to_string(kMaxFailures) +
               " attempts got no closer");
    LOG_WARN("travel: {} ({:.0f} m short)", g_note, straight);
    return;
  }

  if (!PlanAndWalk(here)) {
    if (g_failures >= kMaxFailures) {
      StopLocked(g_note);
      LOG_WARN("travel: {}", g_note);
    }
  }
}

TravelStatus TravelGet() {
  std::lock_guard<std::mutex> lock(g_mutex);
  TravelStatus status;
  status.travelling  = g_travelling;
  status.destination = g_destination;
  status.replans     = g_replans;
  status.failures    = g_failures;
  status.reaching    = g_reaching;
  status.note        = g_note;
  const samp::LocalPed self = samp::ReadLocalPed();
  if (self.valid)
    status.straight_m = Distance2D(Vec3{self.x, self.y, self.z}, g_destination);
  return status;
}

}  // namespace gtabot::act
