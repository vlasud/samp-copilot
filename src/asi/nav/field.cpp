#include "nav/field.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/collision.hpp"
#include "game/peds.hpp"
#include "log.hpp"
#include "nav/grid.hpp"

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
// The ground is read every other cell - a metre - and the cells between
// take the reading beside them. A kerb is not lost at that spacing; the
// reads are quartered.
constexpr int   kGroundStride = 2;
constexpr int   kReadsPerStep = 800;
constexpr int   kExpandPerStep = 6000;
// A pulled leg no longer than this, so the walker replans on a scale it can
// see; and the squeeze out of whatever the start is painted inside.
constexpr float kMaxLeg = 60.0f;
constexpr float kSqueeze = 1.0f;
constexpr float kPedOrigin = 1.0f;

float Away(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

struct Field::Work {
  enum class Phase { kLayout, kPaint, kGround, kClearance, kSearch, kPull, kDone };
  Phase phase = Phase::kLayout;
  Vec3  from, to;
  unsigned long long began_ms = 0;
  Grid  grid;
  float ref_z = 0;

  std::vector<Vec3> tile_centres;
  std::size_t tile_i = 0;
  std::vector<game::col::Body> bodies;
  int ground_at = 0;

  int start = -1, goal = -1;
  Searcher searcher;
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
  Grid& g = w.grid;
  using Phase = Work::Phase;

  const auto finish = [&](const char* why) {
    if (result_.note.empty()) result_.note = why;
    result_.took_ms = static_cast<int>(GetTickCount64() - w.began_ms);
    result_.cells = g.W * g.H;
    result_.expanded = w.searcher.expanded();
    w.phase = Phase::kDone;
    LOG_INFO("field: {} - {}x{} cells, {} tiles, {} ground reads, {} expanded, {} ms: {}",
             result_.ok ? "ok" : "no", g.W, g.H, result_.tiles, result_.ground_reads,
             result_.expanded, result_.took_ms, result_.note);
    return true;
  };

  switch (w.phase) {
    case Phase::kLayout: {
      if (!game::col::Ready() || !game::CallsTrusted())
        return finish("the world does not read");
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
      g.cell = kCell;
      g.x0 = std::floor(minx / kCell) * kCell;
      g.y0 = std::floor(miny / kCell) * kCell;
      const int W = std::min(kMaxSide, static_cast<int>(std::ceil((maxx - g.x0) / kCell)));
      const int H = std::min(kMaxSide, static_cast<int>(std::ceil((maxy - g.y0) / kCell)));
      g.Resize(W, H);
      w.ref_z = w.from.z - kPedOrigin;

      // Tiles across the box, overlapping a little so no seam is bare.
      const float pitch = kTileRadius * 2.0f - kCell * 2.0f;
      for (float cy = g.y0 + kTileRadius; cy - kTileRadius < g.y0 + H * kCell; cy += pitch)
        for (float cx = g.x0 + kTileRadius; cx - kTileRadius < g.x0 + W * kCell; cx += pitch)
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
      float found = 0;
      if (game::GroundBelow(Vec3{c.x, c.y, w.ref_z + 25.0f}, &found) &&
          std::fabs(found - w.ref_z) < 30.0f)
        floor = found;
      game::col::Footprint fp;
      if (!game::col::PaintFootprint(c.x, c.y, floor, kTileRadius, kCell, kBandLow,
                                     kBandHigh, kBodyRadius, {}, w.bodies, &fp))
        return false;
      ++result_.tiles;
      const int dx = static_cast<int>(std::lround((fp.x0 - g.x0) / kCell));
      const int dy = static_cast<int>(std::lround((fp.y0 - g.y0) / kCell));
      for (int iy = 0; iy < fp.side; ++iy)
        for (int ix = 0; ix < fp.side; ++ix) {
          const int fx = ix + dx, fy = iy + dy;
          if (!g.inside(fx, fy)) continue;
          if (fp.blocked[static_cast<std::size_t>(iy) * fp.side + ix])
            g.blocked[g.index(fx, fy)] = 1;
        }
      return false;
    }

    case Phase::kGround: {
      int reads = 0;
      const int n = g.W * g.H;
      while (w.ground_at < n && reads < kReadsPerStep) {
        const int at = w.ground_at++;
        const int ix = at % g.W, iy = at / g.W;
        if (ix % kGroundStride != 0 || iy % kGroundStride != 0) continue;
        if (g.blocked[at]) continue;      // solid: its ground is not walked on
        // From a little above the nearest reading already made, so a slope
        // is followed up and down rather than lost.
        float start_z = w.ref_z;
        if (ix >= kGroundStride && g.known[g.index(ix - kGroundStride, iy)] == 1)
          start_z = g.ground[g.index(ix - kGroundStride, iy)];
        else if (iy >= kGroundStride && g.known[g.index(ix, iy - kGroundStride)] == 1)
          start_z = g.ground[g.index(ix, iy - kGroundStride)];
        const Vec3 c = g.centre(at);
        float found = 0, water = 0;
        ++reads;
        ++result_.ground_reads;
        if (game::GroundBelow(Vec3{c.x, c.y, start_z + 4.0f}, &found) &&
            std::fabs(found - start_z) < 6.0f &&
            !(game::WaterLevel(Vec3{c.x, c.y, found}, &water) && water > found + 0.5f)) {
          g.known[at] = 1;
          g.ground[at] = found;
        } else {
          g.known[at] = 2;
        }
      }
      if (w.ground_at >= n) w.phase = Phase::kClearance;
      return false;
    }

    case Phase::kClearance: {
      // The cells between the readings take the reading beside them.
      for (int iy = 0; iy < g.H; ++iy)
        for (int ix = 0; ix < g.W; ++ix) {
          const int at = g.index(ix, iy);
          if (g.known[at] != 0 || g.blocked[at]) continue;
          const int src = g.index(ix - ix % kGroundStride, iy - iy % kGroundStride);
          g.known[at] = g.known[src];
          g.ground[at] = g.ground[src];
        }
      Chamfer(&g);
      for (int at = 0; at < g.W * g.H; ++at)
        if (!g.passable(at)) ++result_.blocked;

      // Where he stands, squeezed out of whatever the paint put him in.
      int sx, sy, gx, gy;
      if (!g.cell_of(w.from, &sx, &sy)) return finish("the start is outside the field");
      w.start = g.index(sx, sy);
      const int reach = static_cast<int>(std::ceil(kSqueeze / kCell));
      for (int iy = sy - reach; iy <= sy + reach; ++iy)
        for (int ix = sx - reach; ix <= sx + reach; ++ix) {
          if (!g.inside(ix, iy)) continue;
          const int at = g.index(ix, iy);
          if (Away(g.centre(at), w.from) > kSqueeze) continue;
          g.blocked[at] = 0;
          if (g.known[at] != 1) { g.known[at] = 1; g.ground[at] = w.ref_z; }
          if (g.clear[at] < 3) g.clear[at] = 3;
        }
      // The goal, or the nearest cell inside the field to it.
      Vec3 aim = w.to;
      aim.x = std::min(std::max(aim.x, g.x0 + kCell), g.x0 + (g.W - 1) * kCell);
      aim.y = std::min(std::max(aim.y, g.y0 + kCell), g.y0 + (g.H - 1) * kCell);
      g.cell_of(aim, &gx, &gy);
      w.goal = g.index(gx, gy);
      w.searcher.Start(&g, w.start, w.goal, w.to, SearchRules{});
      w.phase = Phase::kSearch;
      return false;
    }

    case Phase::kSearch: {
      if (w.searcher.Step(kExpandPerStep)) w.phase = Phase::kPull;
      return false;
    }

    case Phase::kPull: {
      const int end = w.searcher.end();
      result_.reaches_target = w.searcher.reached_goal() && Away(g.centre(end), w.to) < 2.0f;
      result_.short_by_m = Away(g.centre(end), w.to);
      result_.reached = w.searcher.expanded();
      if (end == w.start) {
        result_.ok = false;
        return finish("no way out of the start cell");
      }
      const std::vector<int> cells = w.searcher.Cells();
      const std::vector<int> pulled = Pull(g, cells, kMaxLeg, SearchRules{}.max_step);

      result_.points.clear();
      result_.points.push_back(w.from);
      for (std::size_t k = 1; k < pulled.size(); ++k) {
        Vec3 p = g.centre(pulled[k]);
        p.z = g.ground[pulled[k]] + kPedOrigin;
        if (k + 1 == pulled.size() && result_.reaches_target) p = w.to;
        result_.points.push_back(p);
      }
      result_.length_m = 0;
      for (std::size_t k = 1; k < result_.points.size(); ++k)
        result_.length_m += Away(result_.points[k - 1], result_.points[k]);
      result_.ok = true;

      // The picture, every other cell, no wider than a screen.
      int stride = 2;
      while (g.W / stride > 200) stride *= 2;
      std::vector<std::uint8_t> on_route(static_cast<std::size_t>(g.W) * g.H, 0);
      for (int at : cells) on_route[at] = 1;
      for (int iy = g.H - 1; iy >= 0; iy -= stride) {
        std::string row;
        for (int ix = 0; ix < g.W; ix += stride) {
          const int at = g.index(ix, iy);
          if (at == w.start)            row += '@';
          else if (at == end)           row += '*';
          else if (on_route[at])        row += 'o';
          else if (g.blocked[at])       row += '#';
          else if (g.known[at] != 1)    row += ' ';
          else                          row += '.';
        }
        result_.picture.push_back(std::move(row));
      }

      char note[240];
      std::snprintf(note, sizeof(note),
                    "collision field %dx%d at %.1f m: %d tiles, %d ground reads, "
                    "%d%% solid or unknown, route %.0f m in %d legs%s",
                    g.W, g.H, kCell, result_.tiles, result_.ground_reads,
                    static_cast<int>(100.0f * result_.blocked / std::max(1, g.W * g.H)),
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
