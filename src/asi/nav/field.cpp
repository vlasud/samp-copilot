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
constexpr float kRoundStart = 80.0f;
// The most field there is: three hundred and sixty metres a side. Beyond
// that the world is not streamed anyway.
constexpr int   kMaxSide = 720;
// Painted in squares of this half-width, each against its own floor,
// because the painter takes one floor height and a street is not one height.
constexpr float kTileRadius = 20.0f;
// The band above the floor a body occupies: over the kerb, under the sign.
constexpr float kBandLow  = 0.30f;
constexpr float kBandHigh = 1.75f;
// Painted as it is, not grown by a body's width. Growing every wall by
// thirty-four centimetres on half-metre cells left no free centre in a
// metre-wide staircase between two handrails, and the courtyard whose only
// way out it was came out sealed. The body's width is honoured instead by
// the clearance every cell already knows - walking beside a wall costs
// more, and the walker keeps him off it - which is how Recast does it.
constexpr float kBodyRadius   = 0.0f;
constexpr float kPersonRadius = 0.45f;
// The ground is read every other cell - a metre - and the cells between
// take the reading beside them. A kerb is not lost at that spacing; the
// reads are quartered.
constexpr int   kGroundStride = 2;
constexpr int   kReadsPerStep = 800;
constexpr int   kExpandPerStep = 6000;
// How far a reading may differ from the height it was looked for at and
// still be the ground rather than something else.
constexpr float kSameLevel = 6.0f;
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
  enum class Phase { kLayout, kGround, kPaint, kClearance, kSearch, kPull, kDone };
  Phase phase = Phase::kLayout;
  Vec3  from, to;
  unsigned long long began_ms = 0;
  Grid  grid;
  float ref_z = 0;

  // Reading the ground, a batch a step, in two passes.
  int ground_at = 0;
  int ground_pass = 0;

  // Painting, a tile a step.
  std::vector<Vec3> tile_centres;
  std::size_t tile_i = 0;
  std::vector<game::col::Body> bodies;

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
      // And a good deal of room round the start in every direction, not only
      // along the line: the way out of the courtyard he stood in was forty
      // metres behind him, opposite the target, and a box drawn round the
      // line alone cut it off.
      // Round both ends, not only the start: the way into the courtyard
      // the target sat in was a staircase forty metres past it, on the very
      // edge of a box drawn round the line, and the field ended short of the
      // target by the length of that detour.
      const float roomx0 = std::min(w.from.x, w.to.x) - kRoundStart;
      const float roomx1 = std::max(w.from.x, w.to.x) + kRoundStart;
      const float roomy0 = std::min(w.from.y, w.to.y) - kRoundStart;
      const float roomy1 = std::max(w.from.y, w.to.y) + kRoundStart;
      float minx = roomx0, maxx = roomx1, miny = roomy0, maxy = roomy1;
      // When that is more field than there is, the room behind the start is
      // what is kept and the rest reaches toward the target - not the other
      // way about. Trimming to forty metres behind the start put the box's
      // edge half a metre short of the staircase out of the courtyard, and
      // whether the field reached depended on which half-metre he stood on.
      const float most = kMaxSide * kCell;
      if (maxx - minx > most) {
        if (w.to.x > w.from.x) { minx = w.from.x - kRoundStart; maxx = minx + most; }
        else                   { maxx = w.from.x + kRoundStart; minx = maxx - most; }
      }
      if (maxy - miny > most) {
        if (w.to.y > w.from.y) { miny = w.from.y - kRoundStart; maxy = miny + most; }
        else                   { maxy = w.from.y + kRoundStart; miny = maxy - most; }
      }
      g.cell = kCell;
      g.x0 = std::floor(minx / kCell) * kCell;
      g.y0 = std::floor(miny / kCell) * kCell;
      const int W = std::min(kMaxSide, static_cast<int>(std::ceil((maxx - g.x0) / kCell)));
      const int H = std::min(kMaxSide, static_cast<int>(std::ceil((maxy - g.y0) / kCell)));
      g.Resize(W, H);
      w.ref_z = w.from.z - kPedOrigin;
      result_.ref_z = w.ref_z;
      result_.box_x0 = g.x0;
      result_.box_y0 = g.y0;
      result_.box_x1 = g.x0 + W * kCell;
      result_.box_y1 = g.y0 + H * kCell;

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
      w.phase = Phase::kGround;
      return false;
    }

    case Phase::kGround: {
      // Two passes. The first reads every reading cell from the height of
      // the start: on a flat city that settles nearly all of them. The
      // second, only for the cells the first could not settle, reads from
      // the height of a neighbour that was settled, so a hillside is
      // followed up. Chaining from the neighbour for every cell was the
      // mistake before: inside a building the chain climbed storey by
      // storey, a tile's floor came out at fifty metres, and at the wall the
      // chain came back down to a street it no longer believed in and left
      // it unknown.
      int reads = 0;
      const int n = g.W * g.H;
      while (w.ground_at < n && reads < kReadsPerStep) {
        const int at = w.ground_at++;
        const int ix = at % g.W, iy = at / g.W;
        if (ix % kGroundStride != 0 || iy % kGroundStride != 0) continue;
        if (w.ground_pass == 1 && g.known[at] != 2) continue;   // settled already
        float start_z = w.ref_z;
        if (w.ground_pass == 1) {
          bool have = false;
          const int dx[4] = {-kGroundStride, kGroundStride, 0, 0};
          const int dy[4] = {0, 0, -kGroundStride, kGroundStride};
          for (int d = 0; d < 4 && !have; ++d)
            if (g.inside(ix + dx[d], iy + dy[d]) &&
                g.known[g.index(ix + dx[d], iy + dy[d])] == 1) {
              start_z = g.ground[g.index(ix + dx[d], iy + dy[d])];
              have = true;
            }
          if (!have) continue;
        }
        const Vec3 c = g.centre(at);
        float found = 0, water = 0;
        ++reads;
        ++result_.ground_reads;
        // From the game's memory directly, not through the guarded call:
        // that one spends a slot per read out of a small reserve per frame
        // and, once the reserve is gone, answers "no ground" - which is how
        // forty per cent of a street came back unknown and walled him in.
        // Objects are left out: a bench top is not a floor.
        if (game::col::GroundBelow(c.x, c.y, start_z + 4.0f, &found, false) &&
            std::fabs(found - start_z) < kSameLevel &&
            !(game::col::WaterAt(c.x, c.y, &water) && water > found + 0.5f)) {
          g.known[at] = 1;
          g.ground[at] = found;
        } else {
          g.known[at] = 2;
        }
      }
      if (w.ground_at >= n) {
        if (w.ground_pass == 0) {
          w.ground_pass = 1;
          w.ground_at = 0;
        } else {
          w.phase = Phase::kPaint;
        }
      }
      return false;
    }

    case Phase::kPaint: {
      if (w.tile_i >= w.tile_centres.size()) {
        w.phase = Phase::kClearance;
        return false;
      }
      const Vec3 c = w.tile_centres[w.tile_i++];
      // The tile's floor is the ground it actually holds: the median of the
      // readings inside it near the start's own level, and of all of them
      // only when there are none near it. A probe from twenty-five metres
      // up found roofs and canopies - floors of three and thirty-eight
      // metres on a street at twelve - and half the tiles were painted in
      // mid-air.
      std::vector<float> heights, close_by;
      {
        int cx, cy;
        const int reach = static_cast<int>(kTileRadius / kCell);
        if (g.cell_of(Vec3{c.x, c.y, 0}, &cx, &cy)) {
          for (int iy = cy - reach; iy < cy + reach; iy += kGroundStride)
            for (int ix = cx - reach; ix < cx + reach; ix += kGroundStride) {
              if (!g.inside(ix, iy)) continue;
              const int at = g.index(ix, iy);
              if (g.known[at] != 1) continue;
              heights.push_back(g.ground[at]);
              if (std::fabs(g.ground[at] - w.ref_z) < kSameLevel)
                close_by.push_back(g.ground[at]);
            }
        }
      }
      std::vector<float>& pick = close_by.empty() ? heights : close_by;
      float floor = w.ref_z;
      if (!pick.empty()) {
        std::nth_element(pick.begin(), pick.begin() + pick.size() / 2, pick.end());
        floor = pick[pick.size() / 2];
      }
      result_.tile_floors.push_back(floor);
      // The floors go with the paint, so a staircase that is the ground
      // under a cell is not a wall at that cell.
      game::col::Floors floors;
      floors.z = g.ground.data();
      floors.known = g.known.data();
      floors.w = g.W;
      floors.h = g.H;
      floors.x0 = g.x0;
      floors.y0 = g.y0;
      floors.cell = kCell;
      game::col::Footprint fp;
      if (!game::col::PaintFootprint(c.x, c.y, floor, kTileRadius, kCell, kBandLow,
                                     kBandHigh, kBodyRadius, {}, w.bodies, &fp, &floors))
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

    case Phase::kClearance: {
      // The cells between the readings take the reading beside them.
      for (int iy = 0; iy < g.H; ++iy)
        for (int ix = 0; ix < g.W; ++ix) {
          const int at = g.index(ix, iy);
          if (g.known[at] != 0) continue;
          const int src = g.index(ix - ix % kGroundStride, iy - iy % kGroundStride);
          g.known[at] = g.known[src];
          g.ground[at] = g.ground[src];
        }
      // The lip of every drop is a wall as far as the clearance is concerned.
      result_.ledges = MarkLedges(&g, SearchRules{}.max_step);
      Chamfer(&g);
      for (int at = 0; at < g.W * g.H; ++at) {
        if (g.blocked[at]) ++result_.blocked;
        else if (g.known[at] != 1) ++result_.unknown;
      }
      // What was read along the line toward the target, for looking at
      // when the route comes out wrong.
      {
        const float span = std::min(80.0f, Away(w.from, w.to));
        const float len = std::max(1.0f, Away(w.from, w.to));
        const float ux = (w.to.x - w.from.x) / len, uy = (w.to.y - w.from.y) / len;
        for (float m = 0; m <= span; m += 1.0f) {
          int ix, iy;
          const Vec3 p{w.from.x + ux * m, w.from.y + uy * m, 0};
          if (!g.cell_of(p, &ix, &iy)) { result_.ground_line.push_back(-2); continue; }
          const int at = g.index(ix, iy);
          result_.ground_line.push_back(g.blocked[at] ? -3 : g.known[at] == 1 ? g.ground[at] : -1);
        }
      }

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
      result_.refused_shut = w.searcher.refused_shut();
      result_.refused_step = w.searcher.refused_step();
      result_.refused_corner = w.searcher.refused_corner();
      result_.tallest_step = w.searcher.tallest_step();
      if (end == w.start) {
        result_.ok = false;
        return finish("no way out of the start cell");
      }
      const std::vector<int> cells = w.searcher.Cells();
      const std::vector<int> pulled = Pull(g, cells, kMaxLeg, SearchRules{}.max_step);
      if (!result_.reaches_target) {
        const int ex = end % g.W, ey = end / g.W;
        char line[120];
        std::snprintf(line, sizeof(line), "end cell ground %.2f clear %.1f",
                      g.ground[end], g.clear[end] / 3.0f);
        result_.end_neighbours.push_back(line);
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            if (!g.inside(ex + dx, ey + dy)) continue;
            const int at = g.index(ex + dx, ey + dy);
            std::snprintf(line, sizeof(line),
                          "%+d,%+d: %s ground %.2f (step %.2f) clear %.1f known %d blocked %d",
                          dx, dy, g.passable(at) ? "free" : "shut", g.ground[at],
                          g.ground[at] - g.ground[end], g.clear[at] / 3.0f,
                          g.known[at], g.blocked[at]);
            result_.end_neighbours.push_back(line);
          }
      }

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
          else if (w.searcher.visited(at)) row += 'x';
          else if (g.known[at] != 1)    row += ' ';
          else                          row += '.';
        }
        result_.picture.push_back(std::move(row));
      }

      char note[260];
      std::snprintf(note, sizeof(note),
                    "collision field %dx%d at %.1f m: %d tiles, %d ground reads, "
                    "%d%% solid, %d%% unknown, %d ledge cells, route %.0f m in %d legs%s",
                    g.W, g.H, kCell, result_.tiles, result_.ground_reads,
                    static_cast<int>(100.0f * result_.blocked / std::max(1, g.W * g.H)),
                    static_cast<int>(100.0f * result_.unknown / std::max(1, g.W * g.H)),
                    result_.ledges,
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

bool Field::At(const Vec3& p, CellInfo* out) const {
  if (w_ == nullptr || out == nullptr) return false;
  const Grid& g = w_->grid;
  if (g.W == 0) return false;
  int ix, iy;
  if (!g.cell_of(p, &ix, &iy)) return false;
  const int at = g.index(ix, iy);
  out->passable = g.passable(at);
  out->blocked = g.blocked[at] != 0;
  out->known = g.known[at];
  out->ground = g.ground[at];
  out->clear = g.clear[at] / 3.0f;
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
