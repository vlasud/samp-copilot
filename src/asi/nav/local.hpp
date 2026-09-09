#pragma once
//
// The picture of the few metres round the character, painted fresh every
// tenth of a second from the same collision the field is drawn from.
//
// The whiskers were lines: a handful of them, at a handful of heights, and
// whatever fell between two of them - a bar, a post, the corner of a car -
// was not there until he walked into it. This is the other way round: every
// quarter-metre cell within six metres is asked whether anything solid
// stands in it between the shin and the head, the way the field asks for a
// whole street, and the answer is a map rather than a few lines. A route is
// checked against it before he walks it, and the way he actually goes is
// chosen on it: the line the plan drew when the line is open, and the
// nearest open heading beside it when it is not.
//
// The picture is pure - grid, bands, queries - so it can be tried on a
// made-up street in a test; the painting of it from the game is beside it
// in local_paint.cpp.
//
#include <cstdint>
#include <vector>

#include "game/collision.hpp"
#include "game/world_query.hpp"
#include "nav/grid.hpp"

namespace gtabot::nav {

struct LocalPicture {
  // Shut where something solid stands from the waist up, where the ground
  // steps more than he can, or where there is no ground read - a drop, or
  // water. Its chamfer is the clearance.
  Grid high;
  // Set where something solid stands below the waist only: a kerb, a bench,
  // a fence to the knee. Open in `high`, so a route may cross it - with a
  // hop.
  std::vector<std::uint8_t> low;
  Vec3  centre;
  float radius = 0;
  bool  ok = false;
  unsigned long long made_ms = 0;
  int   ground_reads = 0, entities = 0;
  bool  starved = false;

  // How far along the segment from a toward b the way is open: the whole
  // length when nothing shut is met, otherwise the distance to the first
  // shut cell. The first `slack` metres are not judged - where he stands
  // is where he stands, inflated walls and all. `low_at`, when given, gets
  // the distance to the first low-only thing on the way, or -1 for none.
  // Past the picture's edge the way is taken as open: the plan saw it.
  float FreeAlong(const Vec3& a, const Vec3& b, float slack, float* low_at) const;
  // Metres from a point to the nearest shut cell, by the chamfer. The
  // picture's own reach for a point off it.
  float Clearance(const Vec3& p) const;
  bool  Shut(const Vec3& p) const;
};

// Paints the picture about `here`: ground read every metre, the two bands,
// ledges, chamfer. `bodies` are painted solid in the high band - the people
// standing about, the pickups not to be stepped on. Game thread; false
// when the world's tables are not readable.
bool PaintLocal(const Vec3& here, float radius,
                const std::vector<game::col::Body>& bodies, LocalPicture* out);

// The bodies for a picture about `here`: everybody within reach but the
// character himself, and the pickups and checkpoint he is not standing in.
// `going_to`, when given, is where he is headed: a pickup there is what he
// was sent to walk into and is not painted at all. Without that the arrow
// he was sent to stand on is a solid disc four and a half metres across
// and he circles it for ever - which is every shop door, every checkpoint
// and the way out of the hospital.
std::vector<game::col::Body> LocalBodies(const Vec3& here, float radius,
                                         const Vec3* going_to = nullptr);

}  // namespace gtabot::nav
