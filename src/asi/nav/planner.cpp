#include "nav/planner.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <queue>
#include <unordered_map>

#include "log.hpp"

namespace gtabot::nav {
namespace {

// Where a ped's origin sits above the ground he stands on.
constexpr float kPedOrigin = 1.0f;
// How far below a point the ground may be before the point is in the air,
// and how far above before the point is underground.
constexpr float kMaxGroundBelow = 3.0f;
constexpr float kMaxGroundAbove = 1.5f;
// Room above the ground for a person: from the knee to over the head.
constexpr float kHeadroomFrom = 0.3f;
constexpr float kHeadroomTo   = 1.9f;
// A person climbs a step and takes a drop; a wall and a ledge are neither.
constexpr float kSampleStep = 1.0f;
constexpr float kMaxClimb   = 1.0f;
constexpr float kMaxDrop    = 1.6f;
// Heights the line between samples is tested at.
constexpr float kKnee  = 0.5f;
constexpr float kChest = 1.3f;

// Joining the route to the graph: how far to look for a node, and how many
// to try before giving up on that end.
constexpr float kJoinRadius = 60.0f;
constexpr std::size_t kJoinCandidates = 8;
// A* stops here; the loaded graph is a few square kilometres and a route
// that needs more than this is not one the character can walk yet.
constexpr int kMaxExpansions = 4000;
// Pulling the route tight: try to skip up to this many nodes at a time, but
// never across a leg longer than this - the game's collision is only sure
// close by, and a long straight line across a city block is usually wrong.
constexpr int   kSmoothLookahead = 3;
constexpr float kSmoothMaxLeg    = 35.0f;
// The whole plan, in calls into the game. Past this, legs are marked
// unverified rather than assumed.
constexpr int kCallBudget = 8000;

int g_calls = 0;
// A wall clock as well as a count. The count bounds how much work is asked
// for; only a clock bounds how long it takes, and everything here runs on the
// game thread, where a long answer is a frame nobody draws and a message
// nobody pumps - which looks exactly like the input being taken away.
unsigned long long g_deadline_ms = 0;
constexpr unsigned long long kBudgetMs = 8;

bool PastDeadline() {
  return g_deadline_ms != 0 && GetTickCount64() > g_deadline_ms;
}

void StartBudget() {
  g_calls = 0;
  g_deadline_ms = GetTickCount64() + kBudgetMs;
}

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

float Distance3D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float dz = b.z - a.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string Metres(float value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f m", value);
  return buffer;
}

bool GroundAt(const Vec3& p, float* ground) {
  if (PastDeadline()) return false;
  ++g_calls;
  return game::GroundBelow(Vec3{p.x, p.y, p.z + kMaxGroundAbove}, ground);
}

// "Nothing is in the way", separated from "nobody could tell us".
//
// A line-of-sight answer is not always to be had: it has to have passed its
// self-check, the calls have to be armed, and the stage that brings it in has
// to have come round. Treating any of those as an obstacle is what made every
// plan in the first twenty-five seconds report "no headroom" about open air.
bool Clear(const Vec3& a, const Vec3& b) {
  if (!game::LineOfSightAvailable()) return true;
  if (PastDeadline()) return true;
  ++g_calls;
  return game::LineClear(a, b);
}

Verdict StandableInner(const Vec3& p) {
  Verdict verdict;
  verdict.where = p;
  if (!game::CallsTrusted()) {
    verdict.why = "game calls are not verified yet";
    return verdict;
  }
  float ground = 0;
  if (!GroundAt(p, &ground)) {
    verdict.why = "no ground - not streamed in, or nothing there";
    return verdict;
  }
  verdict.ground_z = ground;
  const float above = p.z - ground;
  if (above > kMaxGroundBelow) {
    verdict.why = "in the air, ground is " + Metres(above) + " below";
    return verdict;
  }
  // Headroom: a line straight up from just above the ground must be clear.
  if (!Clear(Vec3{p.x, p.y, ground + kHeadroomFrom},
             Vec3{p.x, p.y, ground + kHeadroomTo})) {
    verdict.why = "no headroom";
    return verdict;
  }
  verdict.ok = true;
  return verdict;
}

Verdict WalkableInner(const Vec3& a, const Vec3& b) {
  Verdict verdict;
  Verdict start = StandableInner(a);
  if (!start.ok) {
    start.why = "start: " + start.why;
    return start;
  }

  const float length = Distance2D(a, b);
  const int steps = length < kSampleStep
                        ? 1
                        : static_cast<int>(std::ceil(length / kSampleStep));
  float previous_ground = start.ground_z;
  Vec3  previous{a.x, a.y, start.ground_z};

  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    // Sampled at the height the ground was a step ago, so a slope or a
    // staircase is followed rather than measured from where it started.
    Vec3 sample{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                previous_ground + kPedOrigin};
    const float along = length * t;

    float ground = 0;
    if (!GroundAt(sample, &ground)) {
      verdict.why   = "no ground at " + Metres(along);
      verdict.where = sample;
      verdict.metres = along;
      return verdict;
    }
    const float change = ground - previous_ground;
    if (change > kMaxClimb) {
      verdict.why   = "climb of " + Metres(change) + " at " + Metres(along);
      verdict.where = Vec3{sample.x, sample.y, ground + kPedOrigin};
      verdict.metres = along;
      return verdict;
    }
    if (-change > kMaxDrop) {
      verdict.why   = "drop of " + Metres(-change) + " at " + Metres(along);
      verdict.where = Vec3{sample.x, sample.y, ground + kPedOrigin};
      verdict.metres = along;
      return verdict;
    }
    const Vec3 here{sample.x, sample.y, ground};
    if (!Clear(Vec3{previous.x, previous.y, previous.z + kKnee},
               Vec3{here.x, here.y, here.z + kKnee}) ||
        !Clear(Vec3{previous.x, previous.y, previous.z + kChest},
               Vec3{here.x, here.y, here.z + kChest})) {
      verdict.why   = "blocked at " + Metres(along);
      verdict.where = Vec3{here.x, here.y, ground + kPedOrigin};
      verdict.metres = along;
      return verdict;
    }
    previous        = here;
    previous_ground = ground;
  }
  verdict.ok       = true;
  verdict.ground_z = previous_ground;
  verdict.where    = Vec3{b.x, b.y, previous_ground + kPedOrigin};
  verdict.metres   = length;
  return verdict;
}

// ---- the graph ------------------------------------------------------------

std::uint32_t Key(std::uint16_t area, std::uint16_t index) {
  return (static_cast<std::uint32_t>(area) << 16) | index;
}

struct Open {
  float         priority;
  std::uint32_t key;
  bool operator>(const Open& other) const { return priority > other.priority; }
};

// A* over ped nodes from one node to another, by key. Positions and links
// are read from the game as needed; only loaded areas take part.
bool AStar(const game::PathNode& start, const game::PathNode& goal,
           std::vector<game::PathNode>* route, int* expanded) {
  std::unordered_map<std::uint32_t, float>         best;
  std::unordered_map<std::uint32_t, std::uint32_t> came_from;
  std::unordered_map<std::uint32_t, game::PathNode> known;
  std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;

  const std::uint32_t start_key = Key(start.area, start.index);
  const std::uint32_t goal_key  = Key(goal.area, goal.index);
  best[start_key]  = 0;
  known[start_key] = start;
  known[goal_key]  = goal;
  open.push({Distance3D(start.pos, goal.pos), start_key});
  *expanded = 0;

  while (!open.empty()) {
    const Open current = open.top();
    open.pop();
    if (current.key == goal_key) break;
    if (++*expanded > kMaxExpansions) return false;

    // Copied, not referenced: the map grows inside the loop below, and a
    // rehash would leave a reference to it pointing at freed memory.
    const game::PathNode node = known[current.key];
    const float here = best[current.key];

    game::PathLink links[16];
    const int count = game::ReadLinks(node, links, 16);
    for (int i = 0; i < count; ++i) {
      const std::uint32_t key = Key(links[i].area, links[i].index);
      auto it = known.find(key);
      if (it == known.end()) {
        game::PathNode next;
        if (!game::ReadNode(links[i].area, links[i].index, &next)) continue;
        if (!next.ped) continue;  // a road, not a pavement
        it = known.emplace(key, next).first;
      }
      const float tentative = here + Distance3D(node.pos, it->second.pos);
      auto seen = best.find(key);
      if (seen != best.end() && tentative >= seen->second) continue;
      best[key]      = tentative;
      came_from[key] = current.key;
      open.push({tentative + Distance3D(it->second.pos, goal.pos), key});
    }
  }
  if (came_from.find(goal_key) == came_from.end() && start_key != goal_key)
    return false;

  std::vector<game::PathNode> reversed;
  std::uint32_t at = goal_key;
  while (true) {
    reversed.push_back(known[at]);
    if (at == start_key) break;
    at = came_from[at];
  }
  route->assign(reversed.rbegin(), reversed.rend());
  return true;
}

// The nearest ped node that the character can actually walk to from p.
bool JoinToGraph(const Vec3& p, game::PathNode* joined, std::string* why) {
  const std::vector<game::PathNode> candidates =
      game::PedNodesNear(p, kJoinRadius, kJoinCandidates);
  if (candidates.empty()) {
    *why = "no ped node within " + Metres(kJoinRadius);
    return false;
  }
  std::string last;
  for (const game::PathNode& node : candidates) {
    const Vec3 at{node.pos.x, node.pos.y, node.pos.z + kPedOrigin};
    const Verdict verdict = WalkableInner(p, at);
    if (verdict.ok) {
      *joined = node;
      return true;
    }
    last = verdict.why;
  }
  *why = "none of the " + std::to_string(candidates.size()) +
         " nearest ped nodes is walkable from here (last: " + last + ")";
  return false;
}

Vec3 Lifted(const game::PathNode& node) {
  return Vec3{node.pos.x, node.pos.y, node.pos.z + kPedOrigin};
}

Leg MakeLeg(const Vec3& from, const Vec3& to, bool via_graph) {
  Leg leg;
  leg.from      = from;
  leg.to        = to;
  leg.via_graph = via_graph;
  if (g_calls >= kCallBudget) {
    leg.ok       = true;
    leg.verified = false;
    leg.why      = "unverified - call budget spent";
    return leg;
  }
  const Verdict verdict = WalkableInner(from, to);
  leg.ok       = verdict.ok;
  leg.verified = true;
  leg.why      = verdict.why;
  return leg;
}

std::mutex g_debug_mutex;
DebugState g_debug;

// One place every exit from a plan passes through, so the log always has the
// other half of the pair.
void Report(const Plan& plan, unsigned long long began) {
  bool controls_after = false;
  game::ControlsDisabled(&controls_after);
  LOG_INFO("plan: finished in {} ms, {} calls, {} - controls {}",
           GetTickCount64() - began, plan.game_calls,
           plan.ok ? std::string("ok") : plan.note,
           controls_after ? "DISABLED" : "enabled");
}

}  // namespace

Verdict Standable(const Vec3& p) {
  StartBudget();
  Verdict verdict = StandableInner(p);
  verdict.calls = g_calls;
  return verdict;
}

Verdict Walkable(const Vec3& a, const Vec3& b) {
  StartBudget();
  Verdict verdict = WalkableInner(a, b);
  verdict.calls = g_calls;
  return verdict;
}

Plan PlanPath(const Vec3& from, const Vec3& to) {
  StartBudget();
  Plan plan;
  // Said before the work and after it, because the log flushes every line: a
  // "starting" with no "finished" is the game thread stuck inside this, which
  // no amount of reasoning about it has settled.
  const unsigned long long began = GetTickCount64();
  bool controls_before = false;
  game::ControlsDisabled(&controls_before);
  LOG_INFO("plan: starting ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f}), "
           "controls {}", from.x, from.y, from.z, to.x, to.y, to.z,
           controls_before ? "already disabled" : "enabled");

  const Verdict start = StandableInner(from);
  if (!start.ok) {
    plan.note = "start: " + start.why;
    plan.game_calls = g_calls;
    Report(plan, began);
    return plan;
  }
  const Verdict end = StandableInner(to);
  if (!end.ok) {
    plan.note = "target: " + end.why +
                (Distance2D(from, to) > 250.0f
                     ? " (" + Metres(Distance2D(from, to)) +
                           " away - beyond what the game has streamed in)"
                     : "");
    plan.game_calls = g_calls;
    Report(plan, began);
    return plan;
  }
  const Vec3 a{from.x, from.y, start.ground_z + kPedOrigin};
  const Vec3 b{to.x, to.y, end.ground_z + kPedOrigin};

  // Straight there, when straight there works.
  const Verdict direct = WalkableInner(a, b);
  if (direct.ok) {
    plan.waypoints = {a, b};
    Leg leg;
    leg.from = a;
    leg.to = b;
    leg.ok = true;
    leg.verified = true;
    plan.legs.push_back(leg);
    plan.length_m = Distance2D(a, b);
    plan.ok = true;
    plan.note = "straight line";
    plan.game_calls = g_calls;
    Report(plan, began);
    return plan;
  }

  // Otherwise along the pavements.
  const game::PathLayout& graph = game::CachedPaths();
  if (!graph.valid) {
    plan.note = "straight line " + direct.why +
                ", and the path graph is not available: " + graph.note;
    plan.game_calls = g_calls;
    Report(plan, began);
    return plan;
  }
  game::PathNode start_node, goal_node;
  std::string why;
  if (!JoinToGraph(a, &start_node, &why)) {
    plan.note = "straight line " + direct.why + "; start: " + why;
    plan.game_calls = g_calls;
    Report(plan, began);
    return plan;
  }
  if (!JoinToGraph(b, &goal_node, &why)) {
    plan.note = "straight line " + direct.why + "; target: " + why;
    plan.game_calls = g_calls;
    Report(plan, began);
    return plan;
  }

  std::vector<game::PathNode> route;
  int expanded = 0;
  if (!AStar(start_node, goal_node, &route, &expanded)) {
    plan.note = "straight line " + direct.why +
                "; no route through the loaded graph after " +
                std::to_string(expanded) + " nodes";
    plan.game_calls = g_calls;
    Report(plan, began);
    return plan;
  }
  plan.graph_nodes = static_cast<int>(route.size());

  // Pull it tight: skip a node whenever the straight line to the one after
  // it is walkable, a few nodes at a time, never across a long leg.
  std::vector<Vec3> points;
  points.push_back(a);
  for (const game::PathNode& node : route) points.push_back(Lifted(node));
  points.push_back(b);

  std::vector<Vec3> tight;
  tight.push_back(points.front());
  std::size_t i = 0;
  while (i + 1 < points.size()) {
    std::size_t next = i + 1;
    const std::size_t furthest =
        std::min(points.size() - 1, i + 1 + static_cast<std::size_t>(kSmoothLookahead));
    for (std::size_t j = furthest; j > i + 1; --j) {
      if (Distance2D(points[i], points[j]) > kSmoothMaxLeg) continue;
      if (g_calls >= kCallBudget) break;
      if (WalkableInner(points[i], points[j]).ok) {
        next = j;
        break;
      }
    }
    tight.push_back(points[next]);
    i = next;
  }
  plan.waypoints = tight;

  // Every leg checked against the world. The graph says the pavement is
  // there; the world says whether something is parked on it today.
  bool all_ok = true;
  for (std::size_t k = 0; k + 1 < tight.size(); ++k) {
    const bool via_graph = k != 0 && k + 2 != tight.size();
    Leg leg = MakeLeg(tight[k], tight[k + 1], via_graph);
    plan.length_m += Distance2D(tight[k], tight[k + 1]);
    if (!leg.ok) all_ok = false;
    plan.legs.push_back(std::move(leg));
  }
  plan.ok = all_ok;
  plan.note = all_ok ? "via " + std::to_string(route.size()) +
                           " ped nodes, pulled to " +
                           std::to_string(tight.size() - 1) + " legs"
                     : "a leg of the route is blocked";
  plan.game_calls = g_calls;
  Report(plan, began);
  return plan;
}

void SetDebugPlan(const Vec3& target, const Plan& plan) {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  g_debug.has_target = true;
  g_debug.target     = target;
  g_debug.plan       = plan;
}

void SetDebugFan(std::vector<Vec3> ends, std::vector<bool> ok) {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  g_debug.fan_ends = std::move(ends);
  g_debug.fan_ok   = std::move(ok);
}

void SetDebugNodes(std::vector<game::PathNode> nodes) {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  g_debug.nodes = std::move(nodes);
}

void ClearDebug() {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  g_debug = DebugState{};
}

DebugState GetDebug() {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  return g_debug;
}

}  // namespace gtabot::nav
