// The local picture's queries, on a made-up street.
#include <cmath>

#include "check.hpp"
#include "nav/local.hpp"

namespace {

using gtabot::game::Vec3;
using gtabot::nav::LocalPicture;

// Six metres square at a quarter: 24 cells a side, the ground flat and
// known everywhere.
LocalPicture Street() {
  LocalPicture pic;
  pic.high.cell = 0.25f;
  pic.high.x0 = 0;
  pic.high.y0 = 0;
  pic.high.Resize(24, 24);
  pic.low.assign(24 * 24, 0);
  for (int at = 0; at < 24 * 24; ++at) pic.high.known[at] = 1;
  pic.centre = Vec3{3, 3, 1};
  pic.radius = 3;
  pic.ok = true;
  return pic;
}

bool Near(float a, float b, float within) { return std::fabs(a - b) <= within; }

}  // namespace

void TestLocal() {
  // A wall across the street at x = 3, a metre thick in cells at column 12.
  {
    LocalPicture pic = Street();
    for (int iy = 0; iy < 24; ++iy) pic.high.blocked[pic.high.index(12, iy)] = 1;
    gtabot::nav::Chamfer(&pic.high);
    float low = 0;
    const float free = pic.FreeAlong(Vec3{1, 1.5f, 0}, Vec3{5, 1.5f, 0}, 0.0f, &low);
    check::True(Near(free, 2.0f, 0.13f), "the way is open up to the wall and no further");
    check::Is(low < 0, true, "nothing low on the way to the wall");
    check::True(Near(pic.FreeAlong(Vec3{1, 1.5f, 0}, Vec3{2.5f, 1.5f, 0}, 0.0f, nullptr), 1.5f, 0.01f),
                "a segment that stops short of the wall is open all the way");
    check::True(pic.Clearance(Vec3{2.85f, 1.5f, 0}) < 0.4f, "beside the wall there is no room");
    check::True(pic.Clearance(Vec3{1.0f, 1.5f, 0}) > 1.0f, "two metres from it there is");
    check::Is(pic.Shut(Vec3{3.1f, 2.0f, 0}), true, "the wall's own cell is shut");
  }
  // A kerb across the street at x = 1.5: low only, so the way is open with
  // a hop noted at half a metre.
  {
    LocalPicture pic = Street();
    for (int iy = 0; iy < 24; ++iy) pic.low[pic.high.index(6, iy)] = 1;
    gtabot::nav::Chamfer(&pic.high);
    float low = 0;
    const float free = pic.FreeAlong(Vec3{1, 1.5f, 0}, Vec3{5, 1.5f, 0}, 0.0f, &low);
    check::True(Near(free, 4.0f, 0.01f), "a kerb does not shut the way");
    check::True(Near(low, 0.5f, 0.13f), "but it is noted where it is");
  }
  // Standing in a shut cell - against a wall the paint inflated over him -
  // the first half metre is not judged.
  {
    LocalPicture pic = Street();
    pic.high.blocked[pic.high.index(4, 6)] = 1;
    gtabot::nav::Chamfer(&pic.high);
    check::True(Near(pic.FreeAlong(Vec3{1.1f, 1.6f, 0}, Vec3{4, 1.6f, 0}, 0.5f, nullptr), 2.9f, 0.01f),
                "where he stands is not held against him");
    check::True(pic.FreeAlong(Vec3{1.1f, 1.6f, 0}, Vec3{4, 1.6f, 0}, 0.0f, nullptr) < 0.01f,
                "without the slack it would be");
  }
  // Off the picture the way is taken as open, and no ground is shut.
  {
    LocalPicture pic = Street();
    for (int iy = 0; iy < 24; ++iy) pic.high.known[pic.high.index(20, iy)] = 2;
    gtabot::nav::Chamfer(&pic.high);
    check::True(Near(pic.FreeAlong(Vec3{1, 1, 0}, Vec3{1, 9, 0}, 0.0f, nullptr), 8.0f, 0.01f),
                "past the edge of the picture the way is open");
    check::True(Near(pic.FreeAlong(Vec3{1, 1.5f, 0}, Vec3{5.5f, 1.5f, 0}, 0.0f, nullptr), 4.0f, 0.13f),
                "a cell with no ground read is shut - a drop, or water");
  }
}
