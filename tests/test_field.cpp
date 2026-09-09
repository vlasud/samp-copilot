// The search over a made-up room: a wall with a gap, a ledge, a corner.
//
// The field module reads the world; this is the part after that, tried
// without a world. A route that cuts the corner of a wall, hugs a wall when
// there is room, or climbs a ledge is caught here rather than on a
// pavement.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "check.hpp"
#include "nav/grid.hpp"

using gtabot::nav::Grid;
using gtabot::nav::Vec3;

namespace {

// A 40 x 20 m room at half a metre a cell, all ground, all known.
Grid Room() {
  Grid g;
  g.cell = 0.5f;
  g.x0 = 0;
  g.y0 = 0;
  g.Resize(80, 40);
  for (int at = 0; at < g.W * g.H; ++at) {
    g.known[at] = 1;
    g.ground[at] = 10.0f;
  }
  return g;
}

void Wall(Grid* g, int x0, int y0, int x1, int y1) {
  for (int iy = y0; iy <= y1; ++iy)
    for (int ix = x0; ix <= x1; ++ix)
      if (g->inside(ix, iy)) g->blocked[g->index(ix, iy)] = 1;
}

std::vector<int> Route(Grid* g, const Vec3& from, const Vec3& to, bool* reached) {
  gtabot::nav::MarkLedges(g, gtabot::nav::SearchRules{}.max_step);
  gtabot::nav::Chamfer(g);
  int sx, sy, gx, gy;
  g->cell_of(from, &sx, &sy);
  g->cell_of(to, &gx, &gy);
  gtabot::nav::Searcher s;
  s.Start(g, g->index(sx, sy), g->index(gx, gy), to, gtabot::nav::SearchRules{});
  while (!s.Step(1000)) {}
  *reached = s.reached_goal();
  return s.Cells();
}

int LeastClearance(const Grid& g, const std::vector<int>& cells) {
  int least = 1 << 20;
  for (int at : cells) least = std::min<int>(least, g.clear[at]);
  return least;
}

}  // namespace

void TestField() {
  std::printf("field\n");

  // A ramp of one metre in one, read every metre the way the world is read,
  // is a ramp once the readings are filled in - not a flight of steps a
  // metre high, which is what copying the corner reading made of it, and
  // what walled the character into a storm drain with twenty thousand
  // square metres of floor found round him and nowhere to walk.
  {
    Grid g;
    g.cell = 0.25f;
    g.x0 = 0;
    g.y0 = 0;
    g.Resize(80, 40);          // 20 x 10 m
    const int stride = 4;      // a reading every metre
    for (int iy = 0; iy < g.H; iy += stride)
      for (int ix = 0; ix < g.W; ix += stride) {
        const float x = ix * g.cell;
        const float z = x < 5.0f ? 0.0f : (x < 15.0f ? x - 5.0f : 10.0f);
        const int at = g.index(ix, iy);
        g.known[at] = 1;
        g.ground[at] = z;
      }
    gtabot::nav::SmoothBetweenReadings(&g, stride);
    int unknown = 0;
    float tallest = 0;
    for (int iy = 0; iy < g.H; ++iy)
      for (int ix = 0; ix + 1 < g.W; ++ix) {
        if (g.known[g.index(ix, iy)] != 1) ++unknown;
        tallest = std::max(tallest, std::fabs(g.ground[g.index(ix + 1, iy)] -
                                              g.ground[g.index(ix, iy)]));
      }
    check::Is(unknown, 0, "every cell of the ramp has a floor");
    check::True(tallest < 0.30f, "and no step on it is taller than a kerb");
    check::Is(gtabot::nav::MarkLedges(&g, gtabot::nav::SearchRules{}.max_step), 0,
              "so nothing on it reads as the lip of a drop");
    bool reached = false;
    const std::vector<int> up = Route(&g, Vec3{1.0f, 5.0f, 0}, Vec3{19.0f, 5.0f, 10}, &reached);
    check::True(reached, "and he can walk up it");
  }
  // Where there is no reading anywhere near, there is no floor.
  {
    Grid g;
    g.cell = 0.25f;
    g.Resize(16, 16);
    gtabot::nav::SmoothBetweenReadings(&g, 4);
    int known = 0;
    for (int at = 0; at < g.W * g.H; ++at)
      if (g.known[at] == 1) ++known;
    check::Is(known, 0, "no readings, no floor");
  }

  // A wall across the room with a gap of three cells (1.5 m) in it: the
  // way goes through the gap and nowhere else.
  {
    Grid g = Room();
    Wall(&g, 40, 0, 41, 17);
    Wall(&g, 40, 21, 41, 39);
    bool reached = false;
    const std::vector<int> cells = Route(&g, Vec3{5, 10, 11}, Vec3{35, 10, 11}, &reached);
    check::True(reached, "the gap in the wall is found");
    bool through = false;
    for (int at : cells)
      if (at % g.W == 40 || at % g.W == 41) through = at / g.W >= 18 && at / g.W <= 20;
    check::True(through, "the route goes through the gap, not the wall");
  }

  // Open room, a wall along the south side: with room to spare the route
  // keeps a clear two metres from it rather than hugging it.
  {
    Grid g = Room();
    Wall(&g, 0, 0, 79, 1);
    bool reached = false;
    const std::vector<int> cells = Route(&g, Vec3{2, 3, 11}, Vec3{38, 3, 11}, &reached);
    check::True(reached, "the open room is crossed");
    int lowest_row = 1 << 20;
    for (std::size_t i = 4; i + 4 < cells.size(); ++i)
      lowest_row = std::min(lowest_row, cells[i] / g.W);
    check::True(lowest_row >= 5, "the way keeps off the wall when there is room");
  }

  // A corner: two walls meeting. The diagonal past the corner is not taken
  // through it.
  {
    Grid g = Room();
    Wall(&g, 30, 0, 30, 20);
    Wall(&g, 30, 20, 60, 20);
    bool reached = false;
    const std::vector<int> cells = Route(&g, Vec3{14, 5, 11}, Vec3{17, 15, 11}, &reached);
    check::True(reached, "round the corner");
    check::True(LeastClearance(g, cells) >= 3, "never within a cell of the corner");
  }

  // A ledge: the east half of the room is a metre and a half higher. The
  // step is too tall, so the far side is out of reach and the search says
  // so. A metre exactly is allowed - that is a ramp at forty-five degrees.
  {
    Grid g = Room();
    for (int at = 0; at < g.W * g.H; ++at)
      if (at % g.W >= 40) g.ground[at] = 11.5f;
    bool reached = false;
    const std::vector<int> cells = Route(&g, Vec3{5, 10, 11}, Vec3{35, 10, 12.5f}, &reached);
    check::True(!reached, "a metre and a half of ledge is not stepped up");
    check::True(!cells.empty() && cells.back() % g.W == 38,
                "the nearest reachable cell is a step back from the lip of the ledge");
    int lip_cells_open = 0;
    for (int iy = 0; iy < g.H; ++iy)
      if (g.passable(g.index(39, iy)) || g.passable(g.index(40, iy))) ++lip_cells_open;
    check::Is(lip_cells_open, 0, "both sides of the lip are shut");
  }

  // Pulled tight: an empty room needs one leg, and a room with a wall in
  // the middle needs a few - never the whole chain of cells.
  {
    Grid g = Room();
    bool reached = false;
    const std::vector<int> cells = Route(&g, Vec3{3, 3, 11}, Vec3{36, 16, 11}, &reached);
    const std::vector<int> pulled = gtabot::nav::Pull(g, cells, 60.0f, 0.7f);
    check::Is(static_cast<int>(pulled.size()), 2, "an empty room is one leg");
  }
  {
    Grid g = Room();
    Wall(&g, 40, 6, 41, 39);
    bool reached = false;
    const std::vector<int> cells = Route(&g, Vec3{5, 15, 11}, Vec3{35, 15, 11}, &reached);
    const std::vector<int> pulled = gtabot::nav::Pull(g, cells, 60.0f, 0.7f);
    check::True(reached, "round the end of the wall");
    check::True(pulled.size() >= 3 && pulled.size() <= 6,
                "a wall to go round is a few legs, not the whole chain");
    for (std::size_t i = 1; i < pulled.size(); ++i)
      check::True(gtabot::nav::LineFree(g, pulled[i - 1], pulled[i], 0.7f),
                  "every pulled leg is a clear straight line");
  }
}
