// The painting of the local picture from the game: the part of nav/local
// that reads the world. Game thread.
#include "nav/local.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "game/peds.hpp"
#include "samp/checkpoints.hpp"
#include "samp/objects.hpp"

namespace gtabot::nav {
namespace {

// The same cell as the field, for the same reasons: a fence ten centimetres
// thick paints every cell it passes through, and a metre-wide staircase
// keeps a free middle.
constexpr float kCell = 0.25f;
constexpr int   kStride = 4;            // a ground read every metre
constexpr float kBandLow  = 0.30f;      // below this a thing is the ground: a kerb
constexpr float kWaist    = 0.90f;      // up to this a thing is hopped, above it gone round
constexpr float kBandHigh = 1.75f;      // above this he walks under it
constexpr float kInflate  = kCell * 0.5f;
// How much the floor may rise or fall between two readings a metre apart
// and still be the same floor - the field's own rule, so the two pictures
// agree about what is floor. Where they disagree the walker calls a route
// the plan drew blocked, hands it back, and gets the same route again.
constexpr float kStepChain = 1.5f;
constexpr int   kGroundTries = 4;
constexpr float kPersonRadius = 0.45f;
constexpr float kPickupDisc = 4.5f;     // as the field paints them
constexpr float kPedOrigin = 1.0f;

// The surface a cell's floor is on, read from `from_z`, which is the floor
// of the neighbour it is being chained from. True when there is one within
// a stride's rise of that.
//
// The terrain decides wherever the terrain is there: a bench top, a car
// roof and a crate lid are all solid ground to a collision test, and a
// street where every one of them counted as floor would have him walking
// over the furniture. Where the terrain is not there - a room a server
// built out of objects, a platform, a pier - the objects are the floor,
// because they are all there is to stand on, and a picture that says
// otherwise says the inside of every custom building is a wall.
bool FloorNear(float x, float y, float from_z, float step, float* found) {
  float water = 0;
  if (game::col::WaterAt(x, y, &water) && water > from_z - step + 0.5f) {
    float bed = 0;
    if (!game::col::GroundBelow(x, y, from_z + step + 0.3f, &bed, false) ||
        water > bed + 0.5f)
      return false;   // water over it: not somewhere to walk
  }
  float terrain = 0;
  if (game::col::GroundBelow(x, y, from_z + step + 0.3f, &terrain, false) &&
      std::fabs(terrain - from_z) <= step) {
    *found = terrain;
    return true;
  }
  float solid = 0;
  if (game::col::GroundBelow(x, y, from_z + step + 0.3f, &solid, true) &&
      std::fabs(solid - from_z) <= step) {
    *found = solid;
    return true;
  }
  return false;
}

float Away(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

std::vector<game::col::Body> LocalBodies(const Vec3& here, float radius) {
  std::vector<game::col::Body> bodies;
  for (const game::Ped& who : game::PedsNear(here, radius + 1.0f, 32)) {
    if (Away(who.position, here) < 0.8f) continue;   // himself
    bodies.push_back(game::col::Body{who.position.x, who.position.y,
                                     who.position.z, kPersonRadius});
  }
  // Pickups he is not standing in: the disc the field keeps him out of,
  // so the way he actually goes never cuts through one the route skirted.
  // One he is already inside is not painted - it would wall him in where
  // he stands, and whatever it does it has done.
  for (const samp::Pickup& pickup : samp::PickupsNear(here, radius + kPickupDisc, 64)) {
    if (Away(pickup.at, here) <= kPickupDisc + 0.5f) continue;
    bodies.push_back(game::col::Body{pickup.at.x, pickup.at.y, pickup.at.z, kPickupDisc});
  }
  const samp::Checkpoint cp = samp::CheckpointNow(here);
  if (cp.shown) {
    const float disc = std::max(cp.size + 2.0f, kPickupDisc);
    if (Away(cp.at, here) > disc + 0.5f)
      bodies.push_back(game::col::Body{cp.at.x, cp.at.y, cp.at.z, disc});
  }
  return bodies;
}

bool PaintLocal(const Vec3& here, float radius,
                const std::vector<game::col::Body>& bodies, LocalPicture* out) {
  out->ok = false;
  if (!game::col::Ready() || radius < 1.0f) return false;
  const int side = static_cast<int>(std::ceil(radius * 2.0f / kCell));
  Grid& g = out->high;
  g.cell = kCell;
  g.x0 = here.x - radius;
  g.y0 = here.y - radius;
  g.Resize(side, side);
  out->low.assign(static_cast<std::size_t>(side) * side, 0);
  out->centre = here;
  out->radius = radius;
  out->ground_reads = 0;
  out->entities = 0;
  out->starved = false;
  const float feet = here.z - kPedOrigin;

  // The ground is flooded outward from the cell he is standing on, every
  // metre, each reading taken from the height of a settled neighbour: the
  // floor he is on, followed wherever it goes. Objects are left out - a
  // bench top is not a floor - and water is not ground.
  {
    std::vector<int> queue;
    std::vector<std::uint8_t> tries(static_cast<std::size_t>(side) * side, 0);
    int cx = 0, cy = 0;
    g.cell_of(here, &cx, &cy);
    cx = std::min(std::max(cx - cx % kStride, 0), (side - 1) / kStride * kStride);
    cy = std::min(std::max(cy - cy % kStride, 0), (side - 1) / kStride * kStride);
    const int seed = g.index(cx, cy);
    g.known[seed] = 1;
    g.ground[seed] = feet;   // he is standing on it, whatever a read says
    queue.push_back(seed);
    const int dx[4] = {kStride, -kStride, 0, 0};
    const int dy[4] = {0, 0, kStride, -kStride};
    for (std::size_t head = 0; head < queue.size(); ++head) {
      const int at = queue[head];
      const int ix = at % side, iy = at / side;
      const float from_z = g.ground[at];
      for (int d = 0; d < 4; ++d) {
        const int nx = ix + dx[d], ny = iy + dy[d];
        if (!g.inside(nx, ny)) continue;
        const int next = g.index(nx, ny);
        if (g.known[next] == 1 || tries[next] >= kGroundTries) continue;
        ++tries[next];
        const Vec3 c = g.centre(next);
        float found = 0;
        ++out->ground_reads;
        if (FloorNear(c.x, c.y, from_z, kStepChain, &found)) {
          g.known[next] = 1;
          g.ground[next] = found;
          queue.push_back(next);
        }
      }
    }
    for (int iy = 0; iy < side; iy += kStride)
      for (int ix = 0; ix < side; ix += kStride) {
        const int at = g.index(ix, iy);
        if (g.known[at] != 1) g.known[at] = 2;
      }
  }
  SmoothBetweenReadings(&g, kStride);

  game::col::Floors floors;
  floors.z = g.ground.data();
  floors.known = g.known.data();
  floors.w = side;
  floors.h = side;
  floors.x0 = g.x0;
  floors.y0 = g.y0;
  floors.cell = kCell;

  // Two paints of the same square: from the waist up, which is a wall, and
  // from the shin to the waist, which is a thing to hop when nothing stands
  // above it. Cars count in both.
  game::col::Footprint high, low;
  if (!game::col::PaintFootprint(here.x, here.y, feet, radius, kCell, kWaist, kBandHigh,
                                 kInflate, {}, bodies, &high, &floors, true))
    return false;
  if (!game::col::PaintFootprint(here.x, here.y, feet, radius, kCell, kBandLow, kWaist,
                                 kInflate, {}, {}, &low, &floors, true))
    return false;
  const auto copy = [&](const game::col::Footprint& fp, std::uint8_t* into) {
    const int dx = static_cast<int>(std::lround((fp.x0 - g.x0) / kCell));
    const int dy = static_cast<int>(std::lround((fp.y0 - g.y0) / kCell));
    for (int iy = 0; iy < fp.side; ++iy)
      for (int ix = 0; ix < fp.side; ++ix) {
        const int fx = ix + dx, fy = iy + dy;
        if (!g.inside(fx, fy)) continue;
        if (fp.blocked[static_cast<std::size_t>(iy) * fp.side + ix])
          into[g.index(fx, fy)] = 1;
      }
  };
  copy(high, g.blocked.data());
  copy(low, out->low.data());
  for (std::size_t at = 0; at < out->low.size(); ++at)
    if (g.blocked[at]) out->low[at] = 0;   // a wall is not also a kerb

  out->entities = high.entities + low.entities;
  out->starved = high.starved || low.starved;
  MarkLedges(&g, 1.0f);
  Chamfer(&g);
  out->made_ms = GetTickCount64();
  out->ok = true;
  return true;
}

}  // namespace gtabot::nav
