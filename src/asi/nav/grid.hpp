#pragma once
//
// A field of cells and the searches over it, with nothing of the game in
// them.
//
// The field module reads the world - paints it, asks about the ground - and
// this is the part that comes after: how far each free cell is from a wall,
// the best way across, the few points that way needs. None of it touches
// the game, so all of it can be tried on a made-up room in a test, which is
// how a search that cuts a corner or hugs a wall gets caught on a desk
// rather than on a pavement.
//
#include <cstdint>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::nav {

using game::Vec3;

struct Grid {
  int   W = 0, H = 0;
  float cell = 0.5f;
  float x0 = 0, y0 = 0;
  std::vector<std::uint8_t>  blocked;   // 1: something solid, or too near it
  std::vector<float>         ground;    // the floor under the cell
  std::vector<std::uint8_t>  known;     // 0 not yet, 1 ground read, 2 none
  std::vector<std::uint16_t> clear;     // distance to solid, thirds of a cell

  void Resize(int w, int h);
  int  index(int ix, int iy) const { return iy * W + ix; }
  bool inside(int ix, int iy) const { return ix >= 0 && iy >= 0 && ix < W && iy < H; }
  Vec3 centre(int at) const;
  bool cell_of(const Vec3& p, int* ix, int* iy) const;
  bool passable(int at) const { return !blocked[at] && known[at] == 1; }
};

// Fills `clear`: chamfer 3-4, two passes, the edge of the grid counted as
// solid. Cells that are not passable get nought.
void Chamfer(Grid* g);

// Shuts every cell that stands at the edge of a drop or a rise taller than
// `max_step` - the lip of an embankment, the top of a wall - so that the
// clearance keeps a route off the edge the way it keeps it off a wall.
// Without this the field routed along the very lip of the beach embankment,
// every cell of it free and flat, and the walker went over the side.
// Returns how many cells were shut.
int MarkLedges(Grid* g, float max_step);

// Fills the cells between the ground readings - which are taken every
// `stride` cells - by interpolating the four readings round each, so a
// slope is a slope rather than a flight of steps a stride wide. Cells with
// no reading anywhere near are marked as having none.
void SmoothBetweenReadings(Grid* g, int stride);

struct SearchRules {
  // A metre of rise over a metre of ground - forty-five degrees, what the
  // game's own peds manage on a ramp. Seven-tenths cut the stairs off a
  // beach promenade and walled him onto it.
  float max_step = 1.00f;
  // And the slope he can walk up, in metres of rise per metre of ground.
  // The step above is an absolute one - a kerb, a stair - and at a quarter
  // of a metre to the cell it allowed a rise of four metres for every metre
  // of ground, which is a cliff. The field said the bank of the Los Santos
  // storm drain was walkable on that rule, he set off up it, and slid back
  // down; then again, and again. One to one is forty-five degrees, which is
  // what this game's people manage. It reads right on stairs too: the floor
  // is sampled every metre and smoothed between, so a staircase comes out
  // as the slope it is from a distance - about a third - rather than as its
  // separate steps.
  float max_grade = 1.00f;
  int   clear_wanted = 8;     // cells from a wall before walking is free: two metres at a quarter
  float near_wall_cost = 0.18f;   // per cell short of that, so the total is as it was
  int   max_expand = 0;      // nought: the grid itself is the limit
};

// A* across the grid, in batches so it can sit beside a frame. Ends at the
// goal, or - when the goal is out of reach - remembers the cell that came
// nearest to `aim`.
class Searcher {
 public:
  void Start(const Grid* g, int start, int goal, const Vec3& aim, SearchRules rules);
  // Up to `budget` expansions. True when the search is over.
  bool Step(int budget);
  bool finished() const { return done_; }
  bool reached_goal() const { return reached_; }
  int  end() const;                 // the goal, or the nearest cell
  // The cell it reached that is furthest from the start by walking - the
  // far end of everywhere he can get to. Where the goal cannot be reached
  // and nothing nearer to it can either, this is the way out of wherever
  // he is: a canal is escaped by walking along it, not by standing at the
  // point of it closest to the far side of town.
  int  furthest() const { return furthest_; }
  float furthest_cost() const { return furthest_cost_; }
  // What it cost to reach a cell, or -1 where the search never did.
  float cost(int at) const {
    return closed_.empty() || at < 0 || !closed_[at] ? -1.0f : best_[at];
  }
  float nearest_away() const { return nearest_away_; }
  int  expanded() const { return expanded_; }
  // Why neighbours were turned away, for the times the search ends short:
  // shut (solid or unknown), too tall a step, or the corner of a wall.
  int  refused_shut() const { return refused_shut_; }
  int  refused_step() const { return refused_step_; }
  int  refused_corner() const { return refused_corner_; }
  float tallest_step() const { return tallest_step_; }
  // Whether a cell was expanded, for drawing where the search got to.
  bool visited(int at) const { return !closed_.empty() && closed_[at] != 0; }
  // The way back from `end()` to the start, start first.
  std::vector<int> Cells() const;
  // The way back from any cell it reached, start first.
  std::vector<int> CellsTo(int at) const;

 private:
  struct Open { float f; int at; bool operator<(const Open& o) const { return f > o.f; } };
  const Grid* g_ = nullptr;
  int start_ = -1, goal_ = -1, nearest_ = -1, furthest_ = -1;
  float nearest_away_ = 0, furthest_cost_ = 0;
  Vec3 aim_;
  SearchRules rules_;
  std::vector<float> best_;
  std::vector<int>   came_;
  std::vector<std::uint8_t> closed_;
  std::vector<Open>  heap_;
  bool done_ = false, reached_ = false, started_ = false;
  int  expanded_ = 0;
  int  refused_shut_ = 0, refused_step_ = 0, refused_corner_ = 0;
  float tallest_step_ = 0;
};

// The few cells the way really needs: from each, the furthest later one
// whose straight line stays on free cells with a cell of clearance and no
// ledge along it, and no longer than `max_leg` metres.
std::vector<int> Pull(const Grid& g, const std::vector<int>& cells,
                      float max_leg, float max_step);

// Whether the straight line between two cells is clear the way Pull wants.
bool LineFree(const Grid& g, int a, int b, float max_step);

}  // namespace gtabot::nav
