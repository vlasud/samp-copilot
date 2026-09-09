#include "nav/grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace gtabot::nav {
namespace {

float Away(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

void Grid::Resize(int w, int h) {
  W = w;
  H = h;
  const std::size_t n = static_cast<std::size_t>(w) * h;
  blocked.assign(n, 0);
  ground.assign(n, 0.0f);
  known.assign(n, 0);
  clear.assign(n, 0);
}

Vec3 Grid::centre(int at) const {
  return Vec3{x0 + (at % W + 0.5f) * cell, y0 + (at / W + 0.5f) * cell, 0};
}

bool Grid::cell_of(const Vec3& p, int* ix, int* iy) const {
  *ix = static_cast<int>(std::floor((p.x - x0) / cell));
  *iy = static_cast<int>(std::floor((p.y - y0) / cell));
  return inside(*ix, *iy);
}

void Chamfer(Grid* g) {
  const std::uint16_t unreached = 60000;
  const int W = g->W, H = g->H;
  for (int at = 0; at < W * H; ++at)
    g->clear[at] = g->passable(at) ? unreached : 0;
  for (int iy = 0; iy < H; ++iy)
    for (int ix = 0; ix < W; ++ix) {
      const int at = g->index(ix, iy);
      if (g->clear[at] == 0) continue;
      std::uint16_t best = g->clear[at];
      if (ix == 0 || iy == 0 || ix == W - 1 || iy == H - 1)
        best = std::min<std::uint16_t>(best, 3);
      if (ix > 0)           best = std::min<std::uint16_t>(best, g->clear[at - 1] + 3);
      if (iy > 0)           best = std::min<std::uint16_t>(best, g->clear[at - W] + 3);
      if (ix > 0 && iy > 0) best = std::min<std::uint16_t>(best, g->clear[at - W - 1] + 4);
      if (ix < W - 1 && iy > 0) best = std::min<std::uint16_t>(best, g->clear[at - W + 1] + 4);
      g->clear[at] = best;
    }
  for (int iy = H - 1; iy >= 0; --iy)
    for (int ix = W - 1; ix >= 0; --ix) {
      const int at = g->index(ix, iy);
      if (g->clear[at] == 0) continue;
      std::uint16_t best = g->clear[at];
      if (ix < W - 1)               best = std::min<std::uint16_t>(best, g->clear[at + 1] + 3);
      if (iy < H - 1)               best = std::min<std::uint16_t>(best, g->clear[at + W] + 3);
      if (ix < W - 1 && iy < H - 1) best = std::min<std::uint16_t>(best, g->clear[at + W + 1] + 4);
      if (ix > 0 && iy < H - 1)     best = std::min<std::uint16_t>(best, g->clear[at + W - 1] + 4);
      g->clear[at] = best;
    }
}

// Fills the cells between the readings by interpolating the four readings
// round each, instead of copying the one at its corner.
//
// Copying made every metre of ground a flat plateau with a step at its
// edge, so a ramp of one metre in one - the slipway out of the canals, a
// pavement kerb ramp, a staircase - came out as a flight of metre-high
// steps. The ledge marker shut both sides of every one of them and the
// search refused to climb them, which is how a character standing in a
// storm drain had a hundred and eighty metres of field round him, twenty
// thousand square metres of floor found in it, and a route twenty-seven
// metres long that went nowhere. Interpolated, the same ramp rises a
// quarter of a metre a cell and is simply walkable.
void SmoothBetweenReadings(Grid* g, int stride) {
  const std::vector<float> read_z = g->ground;
  const std::vector<std::uint8_t> read_known = g->known;
  const auto anchor = [&](int ax, int ay, float* z) {
    if (ax >= g->W) ax = (g->W - 1) / stride * stride;
    if (ay >= g->H) ay = (g->H - 1) / stride * stride;
    const int at = ay * g->W + ax;
    if (read_known[at] != 1) return false;
    *z = read_z[at];
    return true;
  };
  for (int iy = 0; iy < g->H; ++iy)
    for (int ix = 0; ix < g->W; ++ix) {
      const int fx = ix % stride, fy = iy % stride;
      if (fx == 0 && fy == 0) continue;          // a reading of its own
      const int ax = ix - fx, ay = iy - fy;
      const float tx = static_cast<float>(fx) / stride;
      const float ty = static_cast<float>(fy) / stride;
      const float weight[4] = {(1 - tx) * (1 - ty), tx * (1 - ty),
                               (1 - tx) * ty, tx * ty};
      const int cx[4] = {ax, ax + stride, ax, ax + stride};
      const int cy[4] = {ay, ay, ay + stride, ay + stride};
      float sum = 0, total = 0;
      for (int k = 0; k < 4; ++k) {
        float z = 0;
        if (weight[k] <= 0 || !anchor(cx[k], cy[k], &z)) continue;
        sum += z * weight[k];
        total += weight[k];
      }
      const int at = g->index(ix, iy);
      if (total > 0) {
        g->known[at] = 1;
        g->ground[at] = sum / total;
      } else {
        g->known[at] = 2;
      }
    }
}

int MarkLedges(Grid* g, float max_step) {
  const int W = g->W, H = g->H;
  std::vector<std::uint8_t> ledge(static_cast<std::size_t>(W) * H, 0);
  const int dx[4] = {1, -1, 0, 0};
  const int dy[4] = {0, 0, 1, -1};
  int shut = 0;
  for (int iy = 0; iy < H; ++iy)
    for (int ix = 0; ix < W; ++ix) {
      const int at = g->index(ix, iy);
      if (!g->passable(at)) continue;
      for (int d = 0; d < 4; ++d) {
        const int nx = ix + dx[d], ny = iy + dy[d];
        if (!g->inside(nx, ny)) continue;
        const int next = g->index(nx, ny);
        if (g->known[next] != 1) continue;
        if (std::fabs(g->ground[next] - g->ground[at]) > max_step) {
          ledge[at] = 1;
          break;
        }
      }
    }
  for (int at = 0; at < W * H; ++at)
    if (ledge[at]) { g->blocked[at] = 1; ++shut; }
  return shut;
}

void Searcher::Start(const Grid* g, int start, int goal, const Vec3& aim,
                     SearchRules rules) {
  g_ = g;
  start_ = start;
  goal_ = goal;
  aim_ = aim;
  rules_ = rules;
  const std::size_t n = static_cast<std::size_t>(g->W) * g->H;
  best_.assign(n, 1e30f);
  came_.assign(n, -1);
  closed_.assign(n, 0);
  heap_.clear();
  best_[start] = 0;
  heap_.push_back(Open{Away(g->centre(start), aim), start});
  nearest_ = start;
  furthest_ = start;
  furthest_cost_ = 0;
  nearest_away_ = Away(g->centre(start), aim);
  done_ = reached_ = false;
  started_ = true;
  expanded_ = 0;
  refused_shut_ = refused_step_ = refused_corner_ = 0;
  tallest_step_ = 0;
}

int Searcher::end() const { return reached_ ? goal_ : nearest_; }

bool Searcher::Step(int budget) {
  if (!started_ || done_) return true;
  const Grid& g = *g_;
  const int step_x[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  const int step_y[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  int done = 0;
  while (!heap_.empty() && done < budget) {
    std::pop_heap(heap_.begin(), heap_.end());
    const Open cur = heap_.back();
    heap_.pop_back();
    if (closed_[cur.at]) continue;
    closed_[cur.at] = 1;
    ++expanded_;
    ++done;
    const float away = Away(g.centre(cur.at), aim_);
    if (away < nearest_away_) { nearest_away_ = away; nearest_ = cur.at; }
    if (best_[cur.at] > furthest_cost_) {
      furthest_cost_ = best_[cur.at];
      furthest_ = cur.at;
    }
    if (cur.at == goal_) { reached_ = true; done_ = true; return true; }
    if (rules_.max_expand > 0 && expanded_ >= rules_.max_expand) { done_ = true; return true; }
    const int hx = cur.at % g.W, hy = cur.at / g.W;
    for (int d = 0; d < 8; ++d) {
      const int nx = hx + step_x[d], ny = hy + step_y[d];
      if (!g.inside(nx, ny)) continue;
      const int next = g.index(nx, ny);
      if (closed_[next]) continue;
      if (!g.passable(next)) { ++refused_shut_; continue; }
      // A diagonal only between two free orthogonal neighbours: a body does
      // not pass through the corner where two walls meet.
      if (d >= 4 && (!g.passable(g.index(hx + step_x[d], hy)) ||
                     !g.passable(g.index(hx, hy + step_y[d])))) {
        ++refused_corner_;
        continue;
      }
      const float rise = std::fabs(g.ground[next] - g.ground[cur.at]);
      if (rise > rules_.max_step) {
        ++refused_step_;
        tallest_step_ = std::max(tallest_step_, rise);
        continue;
      }
      const float run = g.cell * (d >= 4 ? 1.41421f : 1.0f);
      const float clear_cells = g.clear[next] / 3.0f;
      const float penalty = clear_cells < rules_.clear_wanted
                                ? (rules_.clear_wanted - clear_cells) * rules_.near_wall_cost
                                : 0.0f;
      const float tentative = best_[cur.at] + run * (1.0f + penalty);
      if (tentative >= best_[next]) continue;
      best_[next] = tentative;
      came_[next] = cur.at;
      heap_.push_back(Open{tentative + Away(g.centre(next), aim_), next});
      std::push_heap(heap_.begin(), heap_.end());
    }
  }
  if (heap_.empty()) done_ = true;
  return done_;
}

std::vector<int> Searcher::Cells() const {
  std::vector<int> cells;
  for (int at = end(); at != -1; at = came_[at]) {
    cells.push_back(at);
    if (at == start_) break;
  }
  std::reverse(cells.begin(), cells.end());
  return cells;
}

std::vector<int> Searcher::CellsTo(int at) const {
  std::vector<int> back;
  if (!started_ || at < 0 || came_.empty() || closed_.empty() || !closed_[at])
    return back;
  for (int on = at; on != -1; on = came_[on]) {
    back.push_back(on);
    if (on == start_) break;
  }
  std::reverse(back.begin(), back.end());
  return back;
}

bool LineFree(const Grid& g, int a, int b, float max_step) {
  int x0 = a % g.W, y0 = a / g.W;
  const int x1 = b % g.W, y1 = b / g.W;
  const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  float last_ground = g.ground[a];
  while (true) {
    const int at = g.index(x0, y0);
    if (!g.passable(at) || g.clear[at] < 3) return false;
    if (std::fabs(g.ground[at] - last_ground) > max_step) return false;
    last_ground = g.ground[at];
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
  return true;
}

std::vector<int> Pull(const Grid& g, const std::vector<int>& cells,
                      float max_leg, float max_step) {
  std::vector<int> pulled;
  if (cells.empty()) return pulled;
  pulled.push_back(cells[0]);
  std::size_t i = 0;
  while (i + 1 < cells.size()) {
    std::size_t take = i + 1;
    for (std::size_t j = cells.size() - 1; j > i + 1; --j) {
      if (Away(g.centre(cells[i]), g.centre(cells[j])) > max_leg) continue;
      if (LineFree(g, cells[i], cells[j], max_step)) { take = j; break; }
    }
    pulled.push_back(cells[take]);
    i = take;
  }
  return pulled;
}

}  // namespace gtabot::nav
