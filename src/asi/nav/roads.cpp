#include "nav/roads.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

#include "game/paths.hpp"
#include "log.hpp"

namespace gtabot::nav {
namespace {

constexpr float kJoinRadius = 120.0f;     // how far a car may be from a road
constexpr std::size_t kJoinCandidates = 6;
constexpr int kMaxExpansions = 20000;

float Distance(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::uint32_t Key(const game::PathNode& node) {
  return (static_cast<std::uint32_t>(node.area) << 16) | node.index;
}

struct Open {
  float estimate;
  std::uint32_t key;
  bool operator<(const Open& other) const { return estimate > other.estimate; }
};

}  // namespace

Road RoadRoute(const Vec3& from, const Vec3& to) {
  Road road;
  game::ResolvePaths(from);
  const game::Graph graph = game::SnapshotGraph();
  if (!graph.valid) {
    road.note = "the path graph has not been read yet";
    return road;
  }

  const std::vector<game::PathNode> starts =
      graph.VehicleNodesNear(from, kJoinRadius, kJoinCandidates);
  const std::vector<game::PathNode> goals =
      graph.VehicleNodesNear(to, kJoinRadius, kJoinCandidates);
  if (starts.empty() || goals.empty()) {
    road.note = starts.empty() ? "no road within reach of the car"
                               : "no road within reach of the target";
    return road;
  }
  const game::PathNode& start = starts.front();
  const game::PathNode& goal  = goals.front();

  // Plain A* over the road network. No question is put to the world: a link
  // between two road nodes is a road, and the game's own traffic proves it
  // every minute.
  std::unordered_map<std::uint32_t, float> best;
  std::unordered_map<std::uint32_t, std::uint32_t> came_from;
  std::unordered_map<std::uint32_t, game::PathNode> known;
  std::priority_queue<Open> open;

  const std::uint32_t start_key = Key(start), goal_key = Key(goal);
  best[start_key] = 0;
  known[start_key] = start;
  open.push(Open{Distance(start.pos, goal.pos), start_key});

  bool found = false;
  int looked = 0;
  while (!open.empty() && looked < kMaxExpansions) {
    const Open here = open.top();
    open.pop();
    ++looked;
    if (here.key == goal_key) {
      found = true;
      break;
    }
    const auto known_here = known.find(here.key);
    if (known_here == known.end()) continue;
    const game::PathNode node = known_here->second;
    const float cost_here = best[here.key];

    game::PathLink links[16];
    const int count = graph.Links(node, links, 16);
    for (int i = 0; i < count; ++i) {
      const game::PathNode* next = graph.Node(links[i].area, links[i].index);
      if (next == nullptr || next->ped) continue;   // the pavements are not roads
      const std::uint32_t key = Key(*next);
      const float cost = cost_here + Distance(node.pos, next->pos);
      const auto seen = best.find(key);
      if (seen != best.end() && seen->second <= cost) continue;
      best[key] = cost;
      came_from[key] = here.key;
      known[key] = *next;
      open.push(Open{cost + Distance(next->pos, goal.pos), key});
    }
  }
  road.nodes_looked_at = looked;

  if (!found) {
    road.note = looked >= kMaxExpansions
                    ? "the road search gave up after " + std::to_string(looked) +
                          " junctions"
                    : "the roads do not join those two places";
    return road;
  }

  std::vector<Vec3> back;
  std::uint32_t key = goal_key;
  while (true) {
    back.push_back(known[key].pos);
    const auto previous = came_from.find(key);
    if (previous == came_from.end()) break;
    key = previous->second;
  }
  std::reverse(back.begin(), back.end());
  road.points = std::move(back);
  for (std::size_t i = 1; i < road.points.size(); ++i)
    road.length_m += Distance(road.points[i - 1], road.points[i]);

  // The last stretch off the road to whatever was actually asked for.
  road.points.push_back(to);
  road.ok = true;
  road.note = "along " + std::to_string(road.points.size() - 1) +
              " road nodes, " + std::to_string(static_cast<int>(road.length_m)) + " m";
  return road;
}

}  // namespace gtabot::nav
