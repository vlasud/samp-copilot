#include "nav/indoors.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

#include "game/collision.hpp"
#include "log.hpp"
#include "samp/objects.hpp"

namespace gtabot::nav {
namespace {

// The room is drawn from the things in it, not felt for with lines.
//
// Every streamed thing - the walls a server builds a ward out of, the beds,
// the railings, the planters - is painted onto a grid from its own collision
// model: the whole of each thing, in the band of heights a walking person
// occupies, grown by the half-width of his shoulders. What a line at knee
// height stepped over - a bed frame at the shin, a railing post, the rim of
// a planter - is on this map, because the map is made of the things and not
// of a few lines cast among them.
//
// A quarter of a metre a cell: fine enough to find a doorway, coarse enough
// that a room is a few thousand cells.
constexpr float kCell = 0.25f;
constexpr int   kMaxSide = 200;         // 50 m across, whatever the radius
// The band. Below the ankle is what a person steps over without noticing;
// above the head is a lamp.
constexpr float kBandLow  = 0.30f;
constexpr float kBandHigh = 1.75f;
constexpr float kBodyRadius = 0.34f;
// A square with no floor within this of his own is a different storey, or
// the void past a window.
constexpr float kSameFloor = 2.0f;
// How far from a door's position its doorway cells reach, for the record of
// which doors a route goes through. The leaf itself is not painted at all.
constexpr float kDoorDisc = 1.3f;
// Where he already stands is proof enough that a person can; the first
// metre round him is not asked.
constexpr float kSqueezeOut = 1.0f;
// How wide a berth a pickup gets. A pickup fires within about a metre.
constexpr float kPickupDisc = 1.4f;
constexpr int   kMaxFloorReads = 40000;

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

Room MapRoom(const Vec3& from, const Vec3& towards, float radius) {
  Room room;
  room.cell_m = kCell;
  if (!game::CallsTrusted()) {
    room.note = "the world does not read yet";
    return room;
  }

  // The floor he is on. The rooms a server builds float a kilometre above
  // the city, so it is the objects that make the floor, not the terrain.
  float floor_z = from.z - 1.0f;
  float ground = 0;
  if (game::GroundBelow(Vec3{from.x, from.y, from.z + 1.2f}, &ground) &&
      std::fabs(ground - (from.z - 1.0f)) < kSameFloor)
    floor_z = ground;

  float span = radius;
  if (span * 2.0f / kCell > kMaxSide) span = kMaxSide * kCell / 2.0f;

  // The doors first, so their leaves can be left out of the paint. A shut
  // leaf is solid to the painter and open to a person who walks into it;
  // the frame, the wall and the railing beside the door stay exactly as
  // painted, which clearing a disc round the door did not manage.
  std::vector<Vec3> doors;
  std::vector<int> door_models;
  for (const samp::NearObject& door : samp::DoorsNear(from, span + 4.0f, 48)) {
    doors.push_back(door.at);
    bool known = false;
    for (const int m : door_models) if (m == door.model) known = true;
    if (!known) door_models.push_back(door.model);
  }

  game::col::Footprint fp;
  if (!game::col::PaintFootprint(from.x, from.y, floor_z, span, kCell, kBandLow,
                                 kBandHigh, kBodyRadius, door_models, &fp) ||
      fp.side <= 0) {
    room.note = "the world could not be painted";
    return room;
  }
  const int side = fp.side;
  const auto index = [&](int ix, int iy) { return iy * side + ix; };
  const auto centre = [&](int ix, int iy) {
    return Vec3{fp.x0 + (ix + 0.5f) * kCell, fp.y0 + (iy + 0.5f) * kCell, from.z};
  };
  const auto cell_of = [&](const Vec3& at, int* ix, int* iy) {
    *ix = static_cast<int>(std::floor((at.x - fp.x0) / kCell));
    *iy = static_cast<int>(std::floor((at.y - fp.y0) / kCell));
    return *ix >= 0 && *iy >= 0 && *ix < side && *iy < side;
  };

  // Which cells are a doorway, for saying which doors the route went
  // through and for the walker, which pushes a door rather than steering
  // round it. Nothing is cleared: the leaf was never painted.
  std::vector<char> door_cell(static_cast<std::size_t>(side) * side, 0);
  for (const Vec3& door : doors) {
    if (std::fabs(door.z - floor_z) > 3.0f) continue;
    int dx0, dy0;
    if (!cell_of(door, &dx0, &dy0)) continue;
    const int reach = static_cast<int>(std::ceil(kDoorDisc / kCell));
    for (int iy = dy0 - reach; iy <= dy0 + reach; ++iy)
      for (int ix = dx0 - reach; ix <= dx0 + reach; ++ix) {
        if (ix < 0 || iy < 0 || ix >= side || iy >= side) continue;
        if (Distance2D(centre(ix, iy), door) > kDoorDisc) continue;
        door_cell[index(ix, iy)] = 1;
      }
  }

  // Pickups are not floor. A pickup is a thing a server puts on the floor
  // to be walked into on purpose - and a route that crosses one by accident
  // opens whatever it opens: a dialog nobody asked for, a shop, a teleport
  // to another floor. He stood eighty seconds in front of the hospital's
  // information box because the way to the reception ran over its pickup.
  // So they are painted solid, except the one he was sent to.
  int pickups_painted = 0;
  for (const samp::Pickup& pickup : samp::PickupsNear(from, span + 2.0f, 64)) {
    if (std::fabs(pickup.at.z - floor_z) > 3.0f) continue;
    if (Distance2D(pickup.at, towards) <= kPickupDisc + 0.5f) continue;   // the destination
    if (Distance2D(pickup.at, from) <= kSqueezeOut) continue;            // already on it
    int px, py;
    if (!cell_of(pickup.at, &px, &py)) continue;
    const int reach = static_cast<int>(std::ceil(kPickupDisc / kCell));
    for (int iy = py - reach; iy <= py + reach; ++iy)
      for (int ix = px - reach; ix <= px + reach; ++ix) {
        if (ix < 0 || iy < 0 || ix >= side || iy >= side) continue;
        if (Distance2D(centre(ix, iy), pickup.at) > kPickupDisc) continue;
        fp.blocked[index(ix, iy)] = 1;
      }
    ++pickups_painted;
  }

  // Where he stands, and the squeeze out of whatever he is standing in.
  int sx, sy;
  if (!cell_of(from, &sx, &sy)) {
    room.note = "he is outside his own map";
    return room;
  }
  const int start = index(sx, sy);
  {
    const int reach = static_cast<int>(std::ceil(kSqueezeOut / kCell));
    for (int iy = sy - reach; iy <= sy + reach; ++iy)
      for (int ix = sx - reach; ix <= sx + reach; ++ix) {
        if (ix < 0 || iy < 0 || ix >= side || iy >= side) continue;
        if (Distance2D(centre(ix, iy), from) <= kSqueezeOut)
          fp.blocked[index(ix, iy)] = 0;
      }
  }

  // Flooded from under his feet through the free cells. Eight ways, but a
  // diagonal only between two free orthogonal neighbours: a body does not
  // pass through the corner where two walls meet. The floor is asked about
  // only for cells the flood reaches - it is the one read per cell that is
  // still needed, because paint says where the walls are and not where the
  // floor ends.
  std::vector<int> came_from(static_cast<std::size_t>(side) * side, -1);
  std::vector<char> reached(static_cast<std::size_t>(side) * side, 0);
  std::vector<char> floor_known(static_cast<std::size_t>(side) * side, 0);  // 0 ?, 1 yes, 2 no
  std::vector<float> floor(static_cast<std::size_t>(side) * side, floor_z);
  int floor_reads = 0;
  const auto has_floor = [&](int ix, int iy) {
    const int at = index(ix, iy);
    if (floor_known[at] == 0) {
      if (floor_reads >= kMaxFloorReads) return false;
      ++floor_reads;
      const Vec3 c = centre(ix, iy);
      float g = 0;
      const bool ok = game::GroundBelow(Vec3{c.x, c.y, floor_z + 1.5f}, &g) &&
                      std::fabs(g - floor_z) <= kSameFloor;
      floor_known[at] = ok ? 1 : 2;
      if (ok) floor[at] = g;
    }
    return floor_known[at] == 1;
  };

  std::deque<int> queue;
  reached[start] = 1;
  floor_known[start] = 1;
  queue.push_back(start);
  int reached_count = 1;
  const int step_x[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  const int step_y[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  while (!queue.empty()) {
    const int here = queue.front();
    queue.pop_front();
    const int hx = here % side, hy = here / side;
    for (int d = 0; d < 8; ++d) {
      const int nx = hx + step_x[d], ny = hy + step_y[d];
      if (nx < 0 || ny < 0 || nx >= side || ny >= side) continue;
      const int next = index(nx, ny);
      if (reached[next] || fp.blocked[next]) continue;
      if (d >= 4) {
        // No corner-cutting.
        if (fp.blocked[index(hx + step_x[d], hy)] || fp.blocked[index(hx, hy + step_y[d])])
          continue;
      }
      if (!has_floor(nx, ny)) continue;
      reached[next] = 1;
      came_from[next] = here;
      queue.push_back(next);
      ++reached_count;
    }
  }
  room.ok = true;
  room.cells_reached = reached_count;

  // The cell of the room nearest to wherever he was going.
  int best = start;
  float best_away = Distance2D(centre(sx, sy), towards);
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
  room.reaches_target = best_away < kCell * 3.0f;

  // The way there, back along the flood - and which doors it went through.
  std::vector<Vec3> back;
  for (int at = best; at != -1; at = came_from[at]) {
    const int ax = at % side, ay = at / side;
    back.push_back(Vec3{centre(ax, ay).x, centre(ax, ay).y, floor[at] + 1.0f});
    if (door_cell[at]) {
      for (const Vec3& door : doors)
        if (Distance2D(door, centre(ax, ay)) <= kDoorDisc) {
          bool known = false;
          for (const Vec3& d : room.doors)
            if (Distance2D(d, door) < 0.1f) known = true;
          if (!known) room.doors.push_back(door);
        }
    }
    if (at == start) break;
  }
  std::reverse(back.begin(), back.end());
  // Every fourth cell is plenty for the walker, which looks ahead anyway;
  // the ends are kept exactly.
  std::vector<Vec3> thinned;
  for (std::size_t i = 0; i < back.size(); ++i)
    if (i == 0 || i + 1 == back.size() || i % 4 == 0) thinned.push_back(back[i]);
  room.points = std::move(thinned);

  // The picture: what he can reach, what is painted solid, the doors.
  for (int iy = side - 1; iy >= 0; --iy) {
    std::string row;
    for (int ix = 0; ix < side; ++ix) {
      const int at = index(ix, iy);
      if (at == start)                 row += '@';
      else if (at == best)             row += '*';
      else if (reached[at])            row += '.';
      else if (door_cell[at])          row += 'D';
      else if (fp.blocked[at])         row += '#';
      else if (floor_known[at] == 2)   row += ' ';
      else                             row += ',';
    }
    room.picture.push_back(std::move(row));
  }

  char note[300];
  std::snprintf(note, sizeof(note),
                "%d cells of %d reachable; %d things, %d primitives painted %d cells "
                "solid; %d floor reads; the room %s",
                reached_count, side * side, fp.entities, fp.primitives, fp.painted,
                floor_reads,
                room.reaches_target ? "reaches where he was going"
                                    : "ends short of it");
  room.note = note;
  if (!room.reaches_target)
    room.note += " by " + std::to_string(static_cast<int>(best_away)) + " m";
  if (!room.doors.empty())
    room.note += "; through " + std::to_string(room.doors.size()) + " door(s)";
  if (pickups_painted > 0)
    room.note += "; round " + std::to_string(pickups_painted) + " pickup(s)";
  return room;
}

}  // namespace gtabot::nav
