#include "nav/indoors.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

#include "log.hpp"

namespace gtabot::nav {
namespace {

// A square of the grid. Three quarters of a metre is narrower than any
// doorway a person walks through and coarse enough that a room is a few
// thousand of them rather than a few hundred thousand.
constexpr float kCell = 0.75f;
constexpr int   kMaxSide = 96;          // squares across, whatever the radius
constexpr float kKnee  = 0.35f;
constexpr float kChest = 1.05f;
// How far the floor of a square may sit from the floor he is standing on
// before it is a different storey rather than the same room.
constexpr float kSameFloor = 2.0f;
constexpr int   kMaxTests = 60000;

int g_tests = 0;

bool Passable(const Vec3& a, const Vec3& b, float z) {
  if (g_tests >= kMaxTests) return false;
  g_tests += 2;
  const Vec3 knee_a{a.x, a.y, z + kKnee};
  const Vec3 knee_b{b.x, b.y, z + kKnee};
  if (!game::LineClear(knee_a, knee_b, /*include_vehicles=*/false)) return false;
  const Vec3 chest_a{a.x, a.y, z + kChest};
  const Vec3 chest_b{b.x, b.y, z + kChest};
  return game::LineClear(chest_a, chest_b, /*include_vehicles=*/false);
}

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

Room MapRoom(const Vec3& from, const Vec3& towards, float radius) {
  Room room;
  room.cell_m = kCell;
  g_tests = 0;
  if (!game::CallsTrusted()) {
    room.note = "the world does not read yet";
    return room;
  }

  int side = static_cast<int>(radius * 2.0f / kCell) + 1;
  if (side > kMaxSide) side = kMaxSide;
  if (side < 8) side = 8;
  const int middle = side / 2;
  const float base_x = from.x - middle * kCell;
  const float base_y = from.y - middle * kCell;

  const auto centre = [&](int ix, int iy) {
    return Vec3{base_x + ix * kCell, base_y + iy * kCell, from.z};
  };
  const auto index = [&](int ix, int iy) { return iy * side + ix; };

  // The floor of each square, once. A square with no floor within a storey
  // of his own is not part of this room.
  std::vector<float> floor(static_cast<std::size_t>(side) * side, 0);
  std::vector<char> has_floor(static_cast<std::size_t>(side) * side, 0);
  for (int iy = 0; iy < side; ++iy)
    for (int ix = 0; ix < side; ++ix) {
      const Vec3 at = centre(ix, iy);
      float ground = 0;
      ++g_tests;
      if (!game::GroundBelow(Vec3{at.x, at.y, from.z + 1.2f}, &ground)) continue;
      if (std::fabs(ground - (from.z - 1.0f)) > kSameFloor) continue;
      floor[index(ix, iy)] = ground;
      has_floor[index(ix, iy)] = 1;
    }

  // Flooded from under his feet, one square at a time, through whatever a
  // knee and a chest can both pass.
  std::vector<int> came_from(static_cast<std::size_t>(side) * side, -1);
  std::vector<char> reached(static_cast<std::size_t>(side) * side, 0);
  std::deque<int> queue;
  const int start = index(middle, middle);
  reached[start] = 1;
  queue.push_back(start);
  int reached_count = 1;

  const int step_x[4] = {1, -1, 0, 0};
  const int step_y[4] = {0, 0, 1, -1};
  while (!queue.empty() && g_tests < kMaxTests) {
    const int here = queue.front();
    queue.pop_front();
    const int hx = here % side, hy = here / side;
    for (int d = 0; d < 4; ++d) {
      const int nx = hx + step_x[d], ny = hy + step_y[d];
      if (nx < 0 || ny < 0 || nx >= side || ny >= side) continue;
      const int next = index(nx, ny);
      if (reached[next] || !has_floor[next]) continue;
      if (!Passable(centre(hx, hy), centre(nx, ny), floor[here] + 1.0f)) continue;
      reached[next] = 1;
      came_from[next] = here;
      queue.push_back(next);
      ++reached_count;
    }
  }
  room.ok = true;
  room.cells_reached = reached_count;

  // The square of the room nearest to wherever he was going. If the target
  // itself is in the room this is it; if it is not, this is the way out.
  int best = start;
  float best_away = Distance2D(centre(middle, middle), towards);
  for (int iy = 0; iy < side; ++iy)
    for (int ix = 0; ix < side; ++ix) {
      if (!reached[index(ix, iy)]) continue;
      const float away = Distance2D(centre(ix, iy), towards);
      if (away < best_away) {
        best_away = away;
        best = index(ix, iy);
      }
    }

  const int bx = best % side, by = best / side;
  room.way_out = Vec3{centre(bx, by).x, centre(bx, by).y, floor[best] + 1.0f};
  room.way_out_away_m = best_away;
  room.way_out_found = best != start;
  room.reaches_target = best_away < kCell * 1.5f;

  // The way there, back along the flood.
  std::vector<Vec3> back;
  for (int at = best; at != -1; at = came_from[at]) {
    const int ax = at % side, ay = at / side;
    back.push_back(Vec3{centre(ax, ay).x, centre(ax, ay).y, floor[at] + 1.0f});
    if (at == start) break;
  }
  std::reverse(back.begin(), back.end());
  room.points = std::move(back);

  // The picture: what he is standing on, what he can reach, what stopped him.
  for (int iy = side - 1; iy >= 0; --iy) {
    std::string row;
    for (int ix = 0; ix < side; ++ix) {
      const int at = index(ix, iy);
      if (at == start)            row += '@';
      else if (at == best)        row += '*';
      else if (reached[at])       row += '.';
      else if (has_floor[at])     row += '#';
      else                        row += ' ';
    }
    room.picture.push_back(std::move(row));
  }

  room.note = std::to_string(reached_count) + " squares of " +
              std::to_string(side * side) + " reachable, " +
              std::to_string(g_tests) + " questions of the world; the room " +
              (room.reaches_target ? "reaches where he was going"
                                   : "ends " + std::to_string(
                                         static_cast<int>(best_away)) +
                                         " m short of it");
  return room;
}

}  // namespace gtabot::nav
