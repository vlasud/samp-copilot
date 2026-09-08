#include "actions/travel.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "actions/walker.hpp"
#include "game/paths.hpp"
#include "log.hpp"
#include "nav/planner.hpp"
#include "samp/input_state.hpp"
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
// Within this the destination itself is planned to; further out the game
// has not streamed the ground there, and a staging point is what is planned
// to instead.
constexpr float kStreamedRadius = 240.0f;
// Decisions in a row that end no nearer than they started.
constexpr int kMaxFailures = 8;
// Between decisions, so a failed plan is not asked for again the same frame.
constexpr unsigned long long kReplanGapMs = 400;
// How much of a frame the planner may take. Four milliseconds beside a
// sixteen-millisecond frame is a stutter nobody sees.
constexpr int kPlanBudgetMs = 4;
// The journey never waits standing. While a plan is being worked out the
// walker is given somewhere to go in the meantime - the best of the ways
// out from here - and the next leg is planned while the current one is
// still being walked, from this far before its end. A character that stops
// to think every few metres is what "takes a few steps and pauses" is.
constexpr float kPlanAheadMetres = 6.0f;

enum class Phase { kIdle, kWalking };
enum class Aim { kDestination, kStaging, kGreedy };

std::mutex  g_mutex;
bool        g_travelling = false;
Vec3        g_destination;
int         g_replans  = 0;
int         g_failures = 0;
bool        g_reaching = false;
std::string g_note = "idle";
float       g_best_straight = 0;
unsigned long long g_next_plan_ms = 0;
Phase       g_phase = Phase::kIdle;
Aim         g_aim = Aim::kDestination;
Vec3        g_aim_point;
int         g_greedy_legs = 0;
bool        g_bridged = false;   // a leg was issued to cover the current plan
bool        g_height_unknown = false;
nav::Planner g_planner;

// The route the walker was given, for cutting corners on it as he goes.
std::vector<Vec3> g_route;
unsigned long long g_next_cut_ms = 0;
constexpr unsigned long long kCutEveryMs = 700;
constexpr float kCutMaxMetres = 70.0f;
constexpr int   kCutLookahead = 3;

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

void StopLocked(std::string why) {
  g_travelling = false;
  g_phase = Phase::kIdle;
  g_planner.Cancel();
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

// Feeling the way, a leg at a time: the best of the ways out from here, by
// how far it gets and how much of that is toward the target. Either the
// whole plan, when the map has nothing to offer, or the bridge across the
// moment while a plan is being worked out.
bool WalkGreedy(const Vec3& here, bool bridge) {
  const float straight = Distance2D(here, g_destination);
  Vec3 point;
  std::string why;
  if (!nav::BestDirection(here, g_destination, &point, &why)) {
    if (!bridge) {
      g_note = "nowhere to go from here: " + why;
      LOG_WARN("travel: {}", g_note);
      ++g_failures;
      g_next_plan_ms = GetTickCount64() + 1000;
    }
    return false;
  }
  if (!bridge) {
    g_aim = Aim::kGreedy;
    g_aim_point = point;
    g_note = "no route from here - feeling the way toward it";
  }
  ++g_greedy_legs;
  LOG_INFO("travel: {} ({:.0f} m leg toward ({:.0f}, {:.0f}), {:.0f} m to go)",
           bridge ? "moving while the plan is worked out" : g_note,
           Distance2D(here, point), point.x, point.y, straight);
  g_route = {point};
  WalkTo({point});
  g_phase = Phase::kWalking;
  return true;
}

void StartPlan(const Vec3& here, Aim aim, const Vec3& to) {
  g_aim = aim;
  g_aim_point = to;
  g_reaching = aim == Aim::kStaging;
  g_bridged = false;
  g_planner.Start(here, to);
  g_note = aim == Aim::kStaging ? "planning toward the far side" : "planning";
}

// A marker on the map has no height. Once the destination is near enough to
// be streamed in, the ground under it is asked for - from a little above the
// character's own height first, since the land within a couple of hundred
// metres seldom differs by more, and from the sky failing that.
void ResolveHeight(const Vec3& here) {
  float ground = 0;
  if (game::GroundBelow(Vec3{g_destination.x, g_destination.y, here.z + 80.0f},
                        &ground) ||
      game::GroundBelow(Vec3{g_destination.x, g_destination.y, 1000.0f}, &ground)) {
    g_destination.z = ground + 1.0f;
    g_height_unknown = false;
    LOG_INFO("travel: the ground at the target is at {:.1f}", ground);
  }
}

void Decide(const Vec3& here) {
  const float straight = Distance2D(here, g_destination);
  Vec3 staging;
  if (straight <= kStreamedRadius) {
    if (g_height_unknown) ResolveHeight(here);
    StartPlan(here, Aim::kDestination, g_destination);
  } else if (StagingPoint(here, g_destination, &staging)) {
    StartPlan(here, Aim::kStaging, staging);
  } else {
    WalkGreedy(here, false);
  }
}

// Did the last decision get us anywhere? Measured against the best we have
// managed, so shuffling back and forth counts as the failure it is. Returns
// false when the journey has given up.
bool Progress(float straight) {
  if (g_best_straight == 0 || straight < g_best_straight - 1.5f) {
    g_best_straight = straight;
    g_failures = 0;
    return true;
  }
  if (++g_failures >= kMaxFailures) {
    StopLocked("gave up - " + std::to_string(kMaxFailures) +
               " decisions in a row got no closer");
    LOG_WARN("travel: {} ({:.0f} m short)", g_note, straight);
    Stop("the journey gave up");
    return false;
  }
  return true;
}

void OnPlanFinished(const Vec3& here) {
  const nav::Plan& plan = g_planner.result();
  if (plan.ok && plan.waypoints.size() >= 2) {
    nav::SetDebugPlan(g_aim_point, plan);
    // The route itself, so a poor one can be read back off the log: each
    // leg's end, with what it costs beyond walking.
    {
      std::string route;
      char piece[64];
      for (const nav::Leg& leg : plan.legs) {
        std::snprintf(piece, sizeof(piece), " (%.0f,%.0f)%s%s%s%s", leg.to.x,
                      leg.to.y, leg.via_graph ? "g" : "",
                      leg.jumps > 0 ? ("j" + std::to_string(leg.jumps)).c_str() : "",
                      leg.climbs > 0 ? ("c" + std::to_string(leg.climbs)).c_str() : "",
                      leg.drop > 0.5f ? "d" : "");
        route += piece;
      }
      LOG_INFO("plan: route from ({:.0f},{:.0f}):{} [g = along the ped graph, "
               "jN/cN = samples to jump/climb, d = a drop]",
               here.x, here.y, route);
    }
    // Replaces whatever bridge he was walking meanwhile.
    g_route.assign(plan.waypoints.begin() + 1, plan.waypoints.end());
    WalkTo(g_route);
    ++g_replans;
    g_note = g_reaching ? "walking toward the far side" : "walking to the target";
    g_phase = Phase::kWalking;
    g_bridged = false;
    return;
  }
  // The destination could not be planned to from here. A staging point may
  // be, and if not even that, straight at it.
  Vec3 staging;
  if (g_aim == Aim::kDestination && StagingPoint(here, g_destination, &staging) &&
      Distance2D(staging, g_destination) < Distance2D(here, g_destination) - kMinStagingStep) {
    LOG_INFO("travel: no route to the target ({}) - trying the nearest point "
             "toward it", plan.note);
    StartPlan(here, Aim::kStaging, staging);
    return;
  }
  LOG_INFO("travel: no route ({}) - feeling the way", plan.note);
  g_bridged = false;
  WalkGreedy(here, false);
}

}  // namespace

void TravelTo(const Vec3& destination, bool height_unknown) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Stop("replaced by a journey");
  g_destination = destination;
  g_height_unknown = height_unknown;
  g_travelling  = true;
  g_replans     = 0;
  g_failures    = 0;
  g_reaching    = false;
  g_best_straight = 0;
  g_next_plan_ms  = 0;
  g_greedy_legs   = 0;
  g_bridged = false;
  g_phase = Phase::kIdle;
  g_planner.Cancel();
  g_note = "starting";
  LOG_INFO("travel: to ({:.1f}, {:.1f})", destination.x, destination.y);
}

void CancelTravel(const char* why) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_travelling) LOG_INFO("travel: {}", why);
  StopLocked(why);
  Stop(why);
}

// Corners cut on the way. Every so often, while a leg with more legs after
// it is being walked, the straight line from where he is to a waypoint two
// or three legs on is tried against the world as it is at that moment; when
// it is walkable and shorter than the way round, the legs between are
// dropped and he heads for it. What the plan missed - a clear square it
// went round, a corner it kept - is caught here.
void TryCut(const Vec3& here, unsigned long long now, const Status& walk) {
  if (now < g_next_cut_ms || walk.wall || walk.leg < 0) return;
  g_next_cut_ms = now + kCutEveryMs;
  const std::size_t leg = static_cast<std::size_t>(walk.leg);
  if (g_route.size() < 2 || leg + 1 >= g_route.size()) return;
  const std::size_t last =
      std::min(g_route.size() - 1, leg + static_cast<std::size_t>(kCutLookahead));
  int tried = 0;
  for (std::size_t k = last; k > leg && tried < 2; --k) {
    const float straight = Distance2D(here, g_route[k]);
    if (straight > kCutMaxMetres) continue;
    float around = Distance2D(here, g_route[leg]);
    for (std::size_t i = leg + 1; i <= k; ++i)
      around += Distance2D(g_route[i - 1], g_route[i]);
    // Worth it only when it saves something real.
    if (around < straight * 1.15f + 3.0f) continue;
    ++tried;
    const nav::Verdict line = nav::Walkable(here, g_route[k]);
    if (!line.ok || nav::CostOf(line) >= around) continue;
    if (CutTo(k)) {
      LOG_INFO("travel: cutting the corner - straight to leg {} of {}, {:.0f} m "
               "against {:.0f} m round{}", k + 1, g_route.size(), straight, around,
               line.jumps > 0 ? " (" + std::to_string(line.jumps) + " to jump)"
                              : std::string());
    }
    return;
  }
}

void TravelTick() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_travelling) return;

  const unsigned long long now = GetTickCount64();
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
    Stop("arrived");
    LOG_INFO("travel: arrived, {} legs planned along the way{}", g_replans,
             g_greedy_legs > 0 ? " (" + std::to_string(g_greedy_legs) +
                                     " of them felt out)"
                               : "");
    return;
  }

  // The plan, whenever there is one being worked out, whatever he is doing.
  if (g_planner.active() && g_planner.Step(kPlanBudgetMs)) {
    OnPlanFinished(here);
    return;
  }

  const Status walk = Get();
  if (walk.walking) {
    // Held still by SA-MP or the server: nothing to decide until it lets
    // go. Taken by SA-MP's protection, the journey is over: every resumption
    // after the player's F6 and Esc was punished again within a frame, and
    // the third time closed the window for good.
    const char* why = "";
    if (samp::InputLegitimatelyOff(&why)) {
      if (std::strstr(why, "taken the keyboard") != nullptr) {
        StopLocked("stopped - SA-MP took the keyboard; F6 then Esc gives it back, "
                   "the journey is not resumed on its own");
        Stop("SA-MP took the keyboard - the journey is stopped");
        LOG_WARN("travel: {}", g_note);
      }
      return;
    }
    TryCut(here, now, walk);
    // Near the end of a leg that is not the destination, the next one is
    // planned now, so he does not stop to think when he gets there.
    if (!g_planner.active() && g_aim != Aim::kDestination &&
        walk.remaining_m < kPlanAheadMetres && now >= g_next_plan_ms) {
      g_next_plan_ms = now + kReplanGapMs;
      if (!Progress(straight)) return;
      Decide(here);
    }
    return;
  }

  // He is standing. Because a plan is being worked out? Then give him
  // somewhere to go in the meantime - once per plan.
  if (g_planner.active()) {
    if (!g_bridged) {
      g_bridged = true;
      WalkGreedy(here, true);
    }
    return;
  }

  if (g_phase == Phase::kWalking) {
    // He just stopped: there, or against something. A fresh look from where
    // he is now, after a breath.
    g_phase = Phase::kIdle;
    g_next_plan_ms = now + (walk.note == "arrived" ? 50 : kReplanGapMs);
    return;
  }
  if (now < g_next_plan_ms) return;
  g_next_plan_ms = now + kReplanGapMs;
  // Until the world reads under him there is nothing to decide, and waiting
  // for it is not a decision that got nowhere.
  if (!game::CallsTrusted()) {
    static unsigned long long said_ms = 0;
    if (now - said_ms > 5000) {
      said_ms = now;
      LOG_WARN("travel: {}", game::Enabled()
                                 ? "waiting for the world to read under the player"
                                 : "movement is off - nothing will move until it "
                                   "is armed again");
    }
    return;
  }
  if (!Progress(straight)) return;
  Decide(here);
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
