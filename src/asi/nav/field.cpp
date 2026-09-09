#include "nav/field.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <queue>
#include <utility>

#include "game/collision.hpp"
#include "game/peds.hpp"
#include "log.hpp"

namespace gtabot::nav {
namespace {

// Half a metre a cell. A quarter, as the room uses, is four times the cells
// for a street where nothing is narrower than a doorway; a metre misses the
// gap between two parked things. Half a metre is what a body's width is.
constexpr float kCell = 0.5f;
// How far either side of the straight line the field reaches. A detour
// round a whole block is forty metres; anything further is a different
// route, not a detour.
constexpr float kMargin = 40.0f;
// The most field there is: three hundred and sixty metres a side. Beyond
// that the world is not streamed anyway.
constexpr int   kMaxSide = 720;
// Painted in squares of this half-width, each with its own floor, because
// the painter takes one floor height and a street is not one height.
constexpr float kTileRadius = 20.0f;
// The band above the floor a body occupies: over the kerb, under the sign.
constexpr float kBandLow  = 0.30f;
constexpr float kBandHigh = 1.75f;
constexpr float kBodyRadius   = 0.34f;
constexpr float kPersonRadius = 0.45f;
// A step a person takes without thinking; more than this between two cells
// is a ledge, and the field does not climb ledges.
constexpr float kStep = 0.70f;
// The ground is read every other cell - a metre - and the cells between
// take the reading beside them. A kerb is not lost at that spacing; the
// reads are halved four times over.
constexpr int   kGroundStride = 2;
constexpr int   kReadsPerStep = 800;
constexpr int   kExpandPerStep = 6000;
constexpr int   kMaxExpand = 250000;
// Walking within this many cells of something solid costs extra, more the
// nearer: two metres is where a person on a pavement keeps from the wall.
constexpr int   kClearWanted = 4;
constexpr float kNearWallCost = 0.35f;
// A pulled leg no longer than this, so the walker replans on a scale it can
// see, and the squeeze out of whatever the start is inside.
constexpr float kMaxLeg = 60.0f;
constexpr float kSqueeze = 1.0f;
constexpr float kPedOrigin = 1.0f;

float Away(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

struct Open {
  float f;
  int   at;
  bool operator<(const Open& o) const { return f > o.f; }
};

}  // namespace

struct Field::Work {
  enum class Phase { kLayout, kPaint, kGround, kClearance, kSearch, kPull, kDone };
  Phase phase = Phase::kLayout;
  Vec3  from, to;
  unsigned long long began_ms = 0;

  // The grid.
  float x0 = 0, y0 = 0;
  int   W = 0, H = 0;
  std::vector<std::uint8_t>  blocked;
  std::vector<float>         ground;
  std::vector<std::uint8_t>  known;      // 0 not yet, 1 ground, 2 none
  std::vector<std::uint16_t> clear;      // chamfer distance, thirds of a cell
  float ref_z = 0;

  // Painting, a tile a step.
  std::vector<Vec3> tile_centres;
  std::size_t tile_i = 0;
  std::vector<game::col::Body> bodies;

  // Reading the ground, a batch a step.
  int ground_at = 0;                     // the next cell index to consider

  // The search.
  int start = -1, goal = -1, nearest = -1;
  float nearest_away = 0;
  std::vector<float> best;
  std::vector<int>   came;
  std::vector<std::uint8_t> closed;
  std::priority_queue<Open> open;
  int expanded = 0;
  bool search_started = false;

  int index(int ix, int iy) const { return iy * W + ix; }
  bool inside(int ix, int iy) const { return ix >= 0 && iy >= 0 && ix < W && iy < H; }
  Vec3 centre(int at) const {
    return Vec3{x0 + (at % W + 0.5f) * kCell, y0 + (at / W + 0.5f) * kCell, 0};
  }
  bool cell_of(const Vec3& p, int* ix, int* iy) const {
    *ix = static_cast<int>(std::floor((p.x - x0) / kCell));
    *iy = static_cast<int>(std::floor((p.y - y0) / kCell));
    return inside(*ix, *iy);
  }
  bool passable(int at) const { return !blocked[at] && known[at] == 1; }
};

Field::Field() = default;
Field::~Field() { delete w_; }

bool Field::finished() const { return w_ == nullptr || w_->phase == Work::Phase::kDone; }
const FieldResult& Field::result() const { return result_; }

void Field::Start(const Vec3& from, const Vec3& to) {
  delete w_;
  w_ = new Work;
  w_->from = from;
  w_->to = to;
  w_->began_ms = GetTickCount64();
  result_ = FieldResult{};
}

bool Field::Step() {
  if (w_ == nullptr || w_->phase == Work::Phase::kDone) return true;
  Work& w = *w_;
  using Phase = Work::Phase;

  const auto finish = [&](const char* why) {
    result_.note = result_.note.empty() ? why : result_.note;
    result_.took_ms = static_cast<int>(GetTickCount64() - w.began_ms);
    result_.cells = w.W * w.H;
    result_.expanded = w.expanded;
    w.phase = Phase::kDone;
    LOG_INFO("field: {} - {}x{} cells, {} tiles, {} ground reads, {} expanded, {} ms",
             result_.ok ? "ok" : "no", w.W, w.H, result_.tiles, result_.ground_reads,
             w.expanded, result_.took_ms);
    return true;
  };

  switch (w.phase) {
    case Phase::kLayout: {
      if (!game::col::Ready() || !game::CallsTrusted()) {
        result_.note = "the world does not read";
        return finish("the world does not read");
      }
      // The box round both ends with the margin, capped at the biggest
      // field there is - a far target gets a field that reaches toward it
      // and stops, and says by how much.
      float minx = std::min(w.from.x, w.to.x) - kMargin;
      float maxx = std::max(w.from.x, w.to.x) + kMargin;
      float miny = std::min(w.from.y, w.to.y) - kMargin;
      float maxy = std::max(w.from.y, w.to.y) + kMargin;
      const float most = kMaxSide * kCell;
      if (maxx - minx > most) {
        if (w.to.x > w.from.x) { minx = w.from.x - kMargin; maxx = minx + most; }
        else                   { maxx = w.from.x + kMargin; minx = maxx - most; }
      }
      if (maxy - miny > most) {
        if (w.to.y > w.from.y) { miny = w.from.y - kMargin; maxy = miny + most; }
        else                   { maxy = w.from.y + kMargin; miny = maxy - most; }
      }
      w.x0 = std::floor(minx / kCell) * kCell;
      w.y0 = std::floor(miny / kCell) * kCell;
      w.W = static_cast<int>(std::ceil((maxx - w.x0) / kCell));
      w.H = static_cast<int>(std::ceil((maxy - w.y0) / kCell));
      w.W = std::min(w.W, kMaxSide);
      w.H = std::min(w.H, kMaxSide);
      const std::size_t n = static_cast<std::size_t>(w.W) * w.H;
      w.blocked.assign(n, 0);
      w.ground.assign(n, 0.0f);
      w.known.assign(n, 0);
      w.clear.assign(n, 0);
      w.ref_z = w.from.z - kPedOrigin;

      // Tiles across the box, overlapping a little so no seam is bare.
      const float pitch = kTileRadius * 2.0f - kCell * 2.0f;
      for (float cy = w.y0 + kTileRadius; cy - kTileRadius < w.y0 + w.H * kCell; cy += pitch)
        for (float cx = w.x0 + kTileRadius; cx - kTileRadius < w.x0 + w.W * kCell; cx += pitch)
          w.tile_centres.push_back(Vec3{cx, cy, 0});

      // Everybody standing about, painted as they are now.
      const float reach = std::max(Away(w.from, w.to), 60.0f) + kMargin;
      for (const game::Ped& who : game::PedsNear(w.from, reach, 64)) {
        if (Away(who.position, w.from) < 0.8f) continue;     // himself
        w.bodies.push_back(game::col::Body{who.position.x, who.position.y,
                                           who.position.z, kPersonRadius});
      }
      w.phase = Phase::kPaint;
      return false;
    }

    case Phase::kPaint: {
      if (w.tile_i >= w.tile_centres.size()) {
        w.phase = Phase::kGround;
        return false;
      }
      const Vec3 c = w.tile_centres[w.tile_i++];
      // The tile's own floor: the ground under its middle, looked for from
      // well above the start's, so a tile up the hill still finds it.
      float floor = w.ref_z;
      float g = 0;
      if (game::GroundBelow(Vec3{c.x, c.y, w.ref_z + 25.0f}, &g) &&
          std::fabs(g - w.ref_z) < 30.0f)
        floor = g;
      game::col::Footprint fp;
      if (!game::col::PaintFootprint(c.x, c.y, floor, kTileRadius, kCell, kBandLow,
                                     kBandHigh, kBodyRadius, {}, w.bodies, &fp))
        return false;
      ++result_.tiles;
      const int dx = static_cast<int>(std::lround((fp.x0 - w.x0) / kCell));
      const int dy = static_cast<int>(std::lround((fp.y0 - w.y0) / kCell));
      for (int iy = 0; iy < fp.side; ++iy)
        for (int ix = 0; ix < fp.side; ++ix) {
          const int fx = ix + dx, fy = iy + dy;
          if (!w.inside(fx, fy)) continue;
          if (fp.blocked[static_cast<std::size_t>(iy) * fp.side + ix])
            w.blocked[w.index(fx, fy)] = 1;
        }
      return false;
    }

    case Phase::kGround: {
      int reads = 0;
      const int n = w.W * w.H;
      while (w.ground_at < n && reads < kReadsPerStep) {
        const int at = w.ground_at++;
        const int ix = at % w.W, iy = at / w.W;
        if (ix % kGroundStride != 0 || iy % kGroundStride != 0) continue;
        if (w.blocked[at]) continue;      // solid: its ground is not walked on
        // From a little above the nearest reading already made, so a slope
        // is followed up and down rather than lost at six metres.
        float start_z = w.ref_z;
        if (ix >= kGroundStride && w.known[w.index(ix - kGroundStride, iy)] == 1)
          start_z = w.ground[w.index(ix - kGroundStride, iy)];
        else if (iy >= kGroundStride && w.known[w.index(ix, iy - kGroundStride)] == 1)
          start_z = w.ground[w.index(ix, iy - kGroundStride)];
        const Vec3 c = w.centre(at);
        float g = 0, water = 0;
        ++reads;
        ++result_.ground_reads;
        if (game::GroundBelow(Vec3{c.x, c.y, start_z + 4.0f}, &g) &&
            std::fabs(g - start_z) < 6.0f &&
            !(game::WaterLevel(Vec3{c.x, c.y, g}, &water) && water > g + 0.5f)) {
          w.known[at] = 1;
          w.ground[at] = g;
        } else {
          w.known[at] = 2;
        }
      }
      if (w.ground_at >= n) w.phase = Phase::kClearance;
      return false;
    }

    case Phase::kClearance: {
      // The cells between the readings take the reading beside them.
      for (int iy = 0; iy < w.H; ++iy)
        for (int ix = 0; ix < w.W; ++ix) {
          const int at = w.index(ix, iy);
          if (w.known[at] != 0 || w.blocked[at]) continue;
          const int sx = ix - ix % kGroundStride, sy = iy - iy % kGroundStride;
          const int src = w.index(sx, sy);
          w.known[at] = w.known[src];
          w.ground[at] = w.ground[src];
        }
      // Chamfer 3-4: how far every free cell is from anything solid or
      // unknown, in thirds of a cell, two passes.
      const std::uint16_t unreached = 60000;   // `far` is a windows.h macro
      for (int at = 0; at < w.W * w.H; ++at)
        w.clear[at] = w.passable(at) ? unreached : 0;
      for (int iy = 0; iy < w.H; ++iy)
        for (int ix = 0; ix < w.W; ++ix) {
          const int at = w.index(ix, iy);
          if (w.clear[at] == 0) continue;
          std::uint16_t best = w.clear[at];
          if (ix == 0 || iy == 0 || ix == w.W - 1 || iy == w.H - 1) best = std::min<std::uint16_t>(best, 3);
          if (ix > 0)           best = std::min<std::uint16_t>(best, w.clear[at - 1] + 3);
          if (iy > 0)           best = std::min<std::uint16_t>(best, w.clear[at - w.W] + 3);
          if (ix > 0 && iy > 0) best = std::min<std::uint16_t>(best, w.clear[at - w.W - 1] + 4);
          if (ix < w.W - 1 && iy > 0) best = std::min<std::uint16_t>(best, w.clear[at - w.W + 1] + 4);
          w.clear[at] = best;
        }
      for (int iy = w.H - 1; iy >= 0; --iy)
        for (int ix = w.W - 1; ix >= 0; --ix) {
          const int at = w.index(ix, iy);
          if (w.clear[at] == 0) continue;
          std::uint16_t best = w.clear[at];
          if (ix < w.W - 1)           best = std::min<std::uint16_t>(best, w.clear[at + 1] + 3);
          if (iy < w.H - 1)           best = std::min<std::uint16_t>(best, w.clear[at + w.W] + 3);
          if (ix < w.W - 1 && iy < w.H - 1) best = std::min<std::uint16_t>(best, w.clear[at + w.W + 1] + 4);
          if (ix > 0 && iy < w.H - 1) best = std::min<std::uint16_t>(best, w.clear[at + w.W - 1] + 4);
          w.clear[at] = best;
        }
      for (int at = 0; at < w.W * w.H; ++at)
        if (w.blocked[at] || w.known[at] != 1) ++result_.blocked;

      // Where he stands, squeezed out of whatever the paint put him in.
      int sx, sy, gx, gy;
      if (!w.cell_of(w.from, &sx, &sy)) return finish("the start is outside the field");
      w.start = w.index(sx, sy);
      const int reach = static_cast<int>(std::ceil(kSqueeze / kCell));
      for (int iy = sy - reach; iy <= sy + reach; ++iy)
        for (int ix = sx - reach; ix <= sx + reach; ++ix) {
          if (!w.inside(ix, iy)) continue;
          const int at = w.index(ix, iy);
          const Vec3 c = w.centre(at);
          if (Away(c, w.from) > kSqueeze) continue;
          w.blocked[at] = 0;
          if (w.known[at] != 1) { w.known[at] = 1; w.ground[at] = w.ref_z; }
          if (w.clear[at] < 3) w.clear[at] = 3;
        }
      // The goal, or the nearest cell inside the field to it.
      Vec3 aim = w.to;
      aim.x = std::min(std::max(aim.x, w.x0 + kCell), w.x0 + (w.W - 1) * kCell);
      aim.y = std::min(std::max(aim.y, w.y0 + kCell), w.y0 + (w.H - 1) * kCell);
      w.cell_of(aim, &gx, &gy);
      w.goal = w.index(gx, gy);
      w.nearest = w.start;
      w.nearest_away = Away(w.centre(w.start), w.to);
      w.phase = Phase::kSearch;
      return false;
    }

    case Phase::kSearch: {
      if (!w.search_started) {
        w.search_started = true;
        const std::size_t n = static_cast<std::size_t>(w.W) * w.H;
        w.best.assign(n, 1e30f);
        w.came.assign(n, -1);
        w.closed.assign(n, 0);
        w.best[w.start] = 0;
        w.open.push(Open{Away(w.centre(w.start), w.to), w.start});
      }
      const int step_x[8] = {1, -1, 0, 0, 1, 1, -1, -1};
      const int step_y[8] = {0, 0, 1, -1, 1, -1, 1, -1};
      int done = 0;
      while (!w.open.empty() && done < kExpandPerStep) {
        const Open cur = w.open.top();
        w.open.pop();
        if (w.closed[cur.at]) continue;
        w.closed[cur.at] = 1;
        ++w.expanded;
        ++done;
        const Vec3 here = w.centre(cur.at);
        const float away = Away(here, w.to);
        if (away < w.nearest_away) { w.nearest_away = away; w.nearest = cur.at; }
        if (cur.at == w.goal) { w.phase = Phase::kPull; return false; }
        if (w.expanded >= kMaxExpand) { w.phase = Phase::kPull; return false; }
        const int hx = cur.at % w.W, hy = cur.at / w.W;
        for (int d = 0; d < 8; ++d) {
          const int nx = hx + step_x[d], ny = hy + step_y[d];
          if (!w.inside(nx, ny)) continue;
          const int next = w.index(nx, ny);
          if (w.closed[next] || !w.passable(next)) continue;
          if (d >= 4 && (!w.passable(w.index(hx + step_x[d], hy)) ||
                         !w.passable(w.index(hx, hy + step_y[d]))))
            continue;                     // no cutting the corner of a wall
          if (std::fabs(w.ground[next] - w.ground[cur.at]) > kStep) continue;
          const float run = kCell * (d >= 4 ? 1.41421f : 1.0f);
          const float clear_cells = w.clear[next] / 3.0f;
          const float penalty = clear_cells < kClearWanted
                                    ? (kClearWanted - clear_cells) * kNearWallCost : 0.0f;
          const float tentative = w.best[cur.at] + run * (1.0f + penalty);
          if (tentative >= w.best[next]) continue;
          w.best[next] = tentative;
          w.came[next] = cur.at;
          w.open.push(Open{tentative + Away(w.centre(next), w.to), next});
        }
      }
      if (w.open.empty()) w.phase = Phase::kPull;
      return false;
    }

    case Phase::kPull: {
      const int end = w.closed[w.goal] ? w.goal : w.nearest;
      result_.reaches_target = end == w.goal && Away(w.centre(end), w.to) < 2.0f;
      result_.short_by_m = Away(w.centre(end), w.to);
      result_.reached = w.expanded;
      if (end == w.start) {
        result_.ok = false;
        return finish("no way out of the start cell");
      }
      std::vector<int> cells;
      for (int at = end; at != -1; at = w.came[at]) {
        cells.push_back(at);
        if (at == w.start) break;
      }
      std::reverse(cells.begin(), cells.end());

      // Pulled tight over the field itself: from each point the furthest
      // later one whose straight line stays on free cells with a cell of
      // clearance and no ledge along it.
      const auto line_free = [&](int a, int b) {
        int x0 = a % w.W, y0 = a / w.W, x1 = b % w.W, y1 = b / w.W;
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        float last_ground = w.ground[a];
        while (true) {
          const int at = w.index(x0, y0);
          if (!w.passable(at) || w.clear[at] < 3) return false;
          if (std::fabs(w.ground[at] - last_ground) > kStep) return false;
          last_ground = w.ground[at];
          if (x0 == x1 && y0 == y1) break;
          const int e2 = 2 * err;
          if (e2 >= dy) { err += dy; x0 += sx; }
          if (e2 <= dx) { err += dx; y0 += sy; }
        }
        return true;
      };
      std::vector<int> pulled;
      std::size_t i = 0;
      pulled.push_back(cells[0]);
      while (i + 1 < cells.size()) {
        std::size_t take = i + 1;
        for (std::size_t j = cells.size() - 1; j > i + 1; --j) {
          if (Away(w.centre(cells[i]), w.centre(cells[j])) > kMaxLeg) continue;
          if (line_free(cells[i], cells[j])) { take = j; break; }
        }
        pulled.push_back(cells[take]);
        i = take;
      }

      result_.points.clear();
      result_.points.push_back(w.from);
      for (std::size_t k = 1; k < pulled.size(); ++k) {
        Vec3 p = w.centre(pulled[k]);
        p.z = w.ground[pulled[k]] + kPedOrigin;
        if (k + 1 == pulled.size() && result_.reaches_target) p = w.to;
        result_.points.push_back(p);
      }
      result_.length_m = 0;
      for (std::size_t k = 1; k < result_.points.size(); ++k)
        result_.length_m += Away(result_.points[k - 1], result_.points[k]);
      result_.ok = true;

      // The picture, every other cell, no wider than a screen.
      int stride = 2;
      while (w.W / stride > 200) stride *= 2;
      std::vector<std::uint8_t> on_route(static_cast<std::size_t>(w.W) * w.H, 0);
      for (int at : cells) on_route[at] = 1;
      for (int iy = w.H - 1; iy >= 0; iy -= stride) {
        std::string row;
        for (int ix = 0; ix < w.W; ix += stride) {
          const int at = w.index(ix, iy);
          if (at == w.start)            row += '@';
          else if (at == end)           row += '*';
          else if (on_route[at])        row += 'o';
          else if (w.blocked[at])       row += '#';
          else if (w.known[at] != 1)    row += ' ';
          else                          row += '.';
        }
        result_.picture.push_back(std::move(row));
      }

      char note[240];
      std::snprintf(note, sizeof(note),
                    "collision field %dx%d at %.1f m: %d tiles, %d ground reads, "
                    "%d%% solid or unknown, route %.0f m in %d legs%s",
                    w.W, w.H, kCell, result_.tiles, result_.ground_reads,
                    static_cast<int>(100.0f * result_.blocked / std::max(1, w.W * w.H)),
                    result_.length_m, static_cast<int>(result_.points.size()) - 1,
                    result_.reaches_target ? "" : " - ends short of the target");
      result_.note = note;
      if (!result_.reaches_target)
        result_.note += " by " + std::to_string(static_cast<int>(result_.short_by_m)) + " m";
      return finish(note);
    }

    case Phase::kDone:
      return true;
  }
  return true;
}

FieldResult PlanField(const Vec3& from, const Vec3& to, int deadline_ms) {
  Field field;
  field.Start(from, to);
  const unsigned long long until = GetTickCount64() + deadline_ms;
  while (!field.Step()) {
    if (GetTickCount64() > until) {
      FieldResult out = field.result();
      out.note = "ran out of time";
      return out;
    }
  }
  return field.result();
}

}  // namespace gtabot::nav
