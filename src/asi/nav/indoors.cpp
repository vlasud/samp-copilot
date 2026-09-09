#include "nav/indoors.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

#include "log.hpp"
#include "samp/objects.hpp"

namespace gtabot::nav {
namespace {

// A square of the grid. Half a metre against a body a third of a metre wide
// resolves a doorway either way round; three quarters did not, and whether a
// door survived depended on where the character happened to be standing when
// the grid was drawn.
constexpr float kCell = 0.5f;
constexpr int   kMaxSide = 96;          // squares across, whatever the radius
// Above the floor. The knee line is what sees a bed, a bench, a low table -
// everything a server furnishes a room with and everything he was walking
// into - and it has to sit above the slab the floor is built from, which the
// ladder of probes put at well under half a metre. The old lines were
// measured from a metre above the floor and never saw anything below chest
// height at all.
constexpr float kKnee  = 0.45f;
constexpr float kChest = 1.2f;
// How far the floor of a square may sit from the floor he is standing on
// before it is a different storey rather than the same room.
constexpr float kSameFloor = 2.0f;
constexpr int   kMaxTests = 140000;
// Half the width of a person. A square he cannot stand in the middle of is
// not a square he can walk through, however clear the line between it and
// its neighbour looks - which is why a route could be drawn through a gap
// between two beds that his shoulders do not fit into, and why he then spent
// twenty seconds finding that out with his face.
constexpr float kBodyRadius = 0.34f;
// How far from where he stands the body test is waived: a squeeze out of
// whatever he spawned in.
constexpr float kSqueezeOut = 1.1f;
// How near a door has to be to the step being taken for the thing stopping
// him to be that door. A door leaf is about a metre wide.
constexpr float kDoorReach = 1.4f;
constexpr float kDoorHeight = 3.0f;

int g_tests = 0;

// Whether a person standing here would be touching anything: four short
// lines out to the width of his shoulders, at knee and at chest. Asked only
// of squares the flood actually reaches, so it costs a few thousand reads
// for a room rather than a hundred thousand for the whole grid.
// `base_z` is the floor, the same base the passability lines use.
bool BodyFits(const Vec3& at, float base_z) {
  if (g_tests >= kMaxTests) return false;
  const float heights[2] = {kKnee, kChest};
  const float out[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (const float* side : out) {
    for (const float height : heights) {
      g_tests += 1;
      const Vec3 from{at.x, at.y, base_z + height};
      const Vec3 to{at.x + side[0] * kBodyRadius,
                    at.y + side[1] * kBodyRadius, base_z + height};
      if (!game::LineClear(from, to, /*include_vehicles=*/false)) return false;
    }
  }
  return true;
}

// Somewhere in this square he fits, if anywhere does.
//
// Asking only about the centre throws away a doorway whose free space
// happens to straddle two squares - the grid is drawn wherever he was
// standing when the room was mapped, and a door does not move to suit it.
// A ward whose only way out was such a door mapped as a room with no exit,
// which is worse than the coarse test it replaced. So the centre is tried
// first and then a few points inside the square, and whichever fits becomes
// the point the route goes through.
bool FindStanding(const Vec3& centre, float base_z, Vec3* where) {
  const float nudge = kCell * 0.35f;
  const float tries[5][2] = {{0, 0}, {nudge, 0}, {-nudge, 0}, {0, nudge}, {0, -nudge}};
  for (const float* at : tries) {
    const Vec3 point{centre.x + at[0], centre.y + at[1], centre.z};
    if (!BodyFits(point, base_z)) continue;
    *where = point;
    return true;
  }
  return false;
}

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

// Whether what stands between these two squares is a door rather than a wall.
// A door that is shut stops a line of sight exactly the way a wall does, and
// the difference between the two is the whole difference between a room with
// a way out and a room without one - so it is asked of the server's own
// object list rather than of the geometry.
bool DoorBetween(const std::vector<Vec3>& doors, const Vec3& a, const Vec3& b,
                 float floor_z, Vec3* which) {
  const Vec3 middle{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, floor_z};
  for (const Vec3& door : doors) {
    if (std::fabs(door.z - floor_z) > kDoorHeight) continue;
    if (Distance2D(door, middle) <= kDoorReach) {
      if (which) *which = door;
      return true;
    }
  }
  return false;
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
  // The grid is pinned to the world, not to him.
  //
  // Drawing it around wherever he stands means a different lattice every time
  // the room is mapped, so the same doorway is inside a square on one pass and
  // straddling two on the next, and the path he is following changes under his
  // feet every few seconds. That is most of what "he keeps changing his mind"
  // looked like. Snapped to half-metre lines of the world, a second look from
  // ten metres away produces the same squares and the same path.
  const auto snap = [](float value) {
    return std::floor(value / kCell) * kCell;
  };
  const float base_x = snap(from.x - middle * kCell);
  const float base_y = snap(from.y - middle * kCell);

  const auto centre = [&](int ix, int iy) {
    return Vec3{base_x + ix * kCell, base_y + iy * kCell, from.z};
  };
  const auto index = [&](int ix, int iy) { return iy * side + ix; };

  // The doors within reach, once, before any of the feeling starts.
  std::vector<Vec3> doors;
  for (const samp::NearObject& door :
       samp::DoorsNear(from, radius + 4.0f, 48))
    doors.push_back(door.at);

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
  // 0 not asked, 1 he fits, 2 he does not - and where in the square he does.
  std::vector<char> fits(static_cast<std::size_t>(side) * side, 0);
  std::vector<Vec3> stand(static_cast<std::size_t>(side) * side);
  std::vector<int> came_from(static_cast<std::size_t>(side) * side, -1);
  std::vector<char> reached(static_cast<std::size_t>(side) * side, 0);
  std::deque<int> queue;
  const int start = index(middle, middle);
  stand[start] = centre(middle, middle);
  fits[start] = 1;
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
      // Room for his shoulders in the square itself, asked once and kept.
      //
      // Except within reach of where he stands. He is standing there, which
      // is proof enough that a person can, and a character who spawned with
      // his shoulder against a bed and his back to a plant has to be allowed
      // to squeeze out of it before the rule that he may not stand in such a
      // place starts to apply - or the room he is in is one square and the
      // way out of it does not exist.
      if (fits[next] == 0) {
        Vec3 where;
        if (Distance2D(centre(nx, ny), centre(middle, middle)) <= kSqueezeOut) {
          fits[next] = 1;
          stand[next] = centre(nx, ny);
        } else if (FindStanding(centre(nx, ny), floor[next], &where)) {
          fits[next] = 1;
          stand[next] = where;
        } else {
          // Too narrow for his shoulders - or a shut door. A door leaf
          // inside the square is exactly what makes the body test fail, and
          // it is the one obstacle that gets out of the way when he walks
          // into it. Refusing the square here, before the door test ever
          // ran, is how a ward with a door twelve metres down the wall mapped
          // as a room with no way out and sent him into the wall instead.
          Vec3 leaf;
          if (DoorBetween(doors, centre(nx, ny), centre(nx, ny), floor[next],
                          &leaf)) {
            fits[next] = 1;
            stand[next] = centre(nx, ny);
          } else {
            fits[next] = 2;
          }
        }
      }
      if (fits[next] == 2) continue;
      Vec3 door;
      bool through_a_door = false;
      if (!Passable(centre(hx, hy), centre(nx, ny), floor[here])) {
        if (!DoorBetween(doors, centre(hx, hy), centre(nx, ny), floor[here],
                         &door))
          continue;
        through_a_door = true;
      }
      if (through_a_door) room.doors.push_back(door);
      reached[next] = 1;
      came_from[next] = here;
      queue.push_back(next);
      ++reached_count;
    }
  }
  room.ok = true;
  room.cells_reached = reached_count;

  // A room of one square is a bug, not a room, and the bug is in whichever
  // test refused the four squares round him. Say which, and why.
  if (reached_count == 1) {
    std::string why;
    for (int d = 0; d < 4; ++d) {
      const int nx = middle + step_x[d], ny = middle + step_y[d];
      const int next = index(nx, ny);
      const Vec3 at = centre(nx, ny);
      char line[200];
      if (!has_floor[next]) {
        std::snprintf(line, sizeof(line), " [%+d,%+d: no floor]", step_x[d], step_y[d]);
      } else {
        const float base = floor[next];
        const float heights[2] = {kKnee, kChest};
        const char* names[2] = {"knee", "chest"};
        const float out[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        std::string failed;
        for (int h = 0; h < 2; ++h)
          for (int o = 0; o < 4; ++o) {
            const Vec3 a{at.x, at.y, base + heights[h]};
            const Vec3 b{at.x + out[o][0] * kBodyRadius,
                         at.y + out[o][1] * kBodyRadius, base + heights[h]};
            if (!game::LineClear(a, b, false))
              failed += std::string(" ") + names[h] + (o == 0 ? "+x" : o == 1 ? "-x" : o == 2 ? "+y" : "-y");
          }
        std::snprintf(line, sizeof(line), " [%+d,%+d at (%.2f,%.2f) floor %.2f: fits=%d passable=%d blocked:%s]",
                      step_x[d], step_y[d], at.x, at.y, base, fits[next],
                      Passable(centre(middle, middle), at, floor[start]) ? 1 : 0,
                      failed.empty() ? " nothing" : failed.c_str());
      }
      why += line;
    }
    room.note = "one square:" + why;
    LOG_WARN("room: {}", room.note);
  }

  // The square of the room nearest to wherever he was going. If the target
  // itself is in the room this is it; if it is not, this is the way out.
  int best = start;
  float best_away = Distance2D(centre(middle, middle), towards);
  for (int iy = 0; iy < side; ++iy)
    for (int ix = 0; ix < side; ++ix) {
      if (!reached[index(ix, iy)]) continue;
      const float away = Distance2D(stand[index(ix, iy)], towards);
      if (away < best_away) {
        best_away = away;
        best = index(ix, iy);
      }
    }

  room.way_out = Vec3{stand[best].x, stand[best].y, floor[best] + 1.0f};
  room.way_out_away_m = best_away;
  room.way_out_found = best != start;
  room.reaches_target = best_away < kCell * 1.5f;

  // The way there, back along the flood.
  std::vector<Vec3> back;
  for (int at = best; at != -1; at = came_from[at]) {
    back.push_back(Vec3{stand[at].x, stand[at].y, floor[at] + 1.0f});
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
      else if (fits[at] == 2)     row += 'o';   // floor, but too narrow for him
      else if (has_floor[at])     row += '#';
      else                        row += ' ';
    }
    room.picture.push_back(std::move(row));
  }

  // The same door is found from both of its sides; say it once.
  std::sort(room.doors.begin(), room.doors.end(),
            [](const Vec3& a, const Vec3& b) {
              if (a.x != b.x) return a.x < b.x;
              if (a.y != b.y) return a.y < b.y;
              return a.z < b.z;
            });
  room.doors.erase(std::unique(room.doors.begin(), room.doors.end(),
                               [](const Vec3& a, const Vec3& b) {
                                 return Distance2D(a, b) < 0.1f &&
                                        std::fabs(a.z - b.z) < 0.1f;
                               }),
                   room.doors.end());

  room.note = std::to_string(reached_count) + " squares of " +
              std::to_string(side * side) + " reachable, " +
              std::to_string(g_tests) + " questions of the world; the room " +
              (room.reaches_target ? "reaches where he was going"
                                   : "ends " + std::to_string(
                                         static_cast<int>(best_away)) +
                                         " m short of it") +
              (room.doors.empty()
                   ? ""
                   : "; " + std::to_string(room.doors.size()) +
                         " of the ways between squares are doors, which is "
                         "what he has to walk into rather than round");
  return room;
}

}  // namespace gtabot::nav
