#include "nav/field.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/collision.hpp"
#include "game/peds.hpp"
#include "samp/checkpoints.hpp"
#include "samp/objects.hpp"
#include "log.hpp"
#include "nav/grid.hpp"
#include "nav/planner.hpp"

namespace gtabot::nav {
namespace {

// A quarter of a metre a cell, as the room uses. Half a metre was tried
// first, for a street, and it was too coarse both ways at once: a fence ten
// centimetres thick fell between the cell centres and was invisible, and a
// metre-wide staircase between two handrails had no free centre in it.
// Recast puts three or four cells across a body for the same reason.
constexpr float kCell = 0.25f;
// How far either side of the straight line the field reaches. A detour
// round a whole block is forty metres; anything further is a different
// route, not a detour.
constexpr float kMargin = 40.0f;
constexpr float kRoundStart = 60.0f;
// And the least: enough to step round a parked car either side.
constexpr float kLeastRound = 12.0f;
// The most field there is: two hundred and forty metres a side at this
// cell. A journey is staged and replanned as the world streams in anyway,
// so a plan need only reach the next stage - but each stage is chosen
// greedily, for the reachable point nearest the target, and a greedy choice
// made from a smaller box is wrong more often: six hundred and forty-one
// metres of straight line once cost fifteen hundred of walking. Fewer,
// longer stages are fewer chances to choose badly.
constexpr int   kMaxSide = 960;
// Painted in squares of this half-width, each against its own floor,
// because the painter takes one floor height and a street is not one height.
constexpr float kTileRadius = 20.0f;
// The band above the floor a body occupies: over the kerb, under the sign.
constexpr float kBandLow  = 0.30f;
constexpr float kBandHigh = 1.75f;
// Grown by half a cell, and no more: enough that a thing thinner than a
// cell paints every cell it passes through, which a fence ten centimetres
// thick otherwise did not, and not so much that a metre-wide staircase
// between two handrails loses its free middle. The body's own width is
// honoured by the clearance every cell knows - walking beside a wall costs
// more, and the walker keeps him off it - which is how Recast does it.
constexpr float kBodyRadius   = kCell * 0.5f;
constexpr float kPersonRadius = 0.45f;
// The ground is read every fourth cell - a metre - and the cells between
// take the reading beside them. A kerb is not lost at that spacing, and the
// reads are a sixteenth of what every cell would cost.
// The ground is read every fourth cell - a metre - out of doors, where
// nothing he must fit through is narrower than a pavement. In a small
// place it is read every other cell - half a metre - because a doorway is
// a metre wide and a reading every metre falls on either side of it: three
// quarters of a hospital came back unknown that way, its rooms cut off
// from their own corridor, and the routes drawn across it were nonsense.
constexpr int   kGroundStride = 4;
constexpr int   kCloseStride  = 2;
// A box this small is a room, a yard, a shop - somewhere to read closely.
constexpr int   kCloseSide = 260;          // sixty-five metres
// How much the floor may rise or fall between two readings a metre apart
// and still be the same floor. A staircase at forty-five degrees is one
// metre in one; half a metre more allows for a steep one and for the
// reading landing on the edge of a step. Whether he can actually take the
// step is the search's business, not the reading's.
constexpr float kStepChain = 1.5f;
// How many reads a cell that will not settle may cost before it is left
// alone: one from each side, so a hilltop refused from the steep side can
// still be reached from the gentle one.
constexpr int   kGroundTries = 4;
constexpr int   kReadsPerStep = 800;
constexpr int   kExpandPerStep = 6000;
// How far a reading may differ from the height it was looked for at and
// still be the ground rather than something else.
constexpr float kSameLevel = 6.0f;
// How much nearer the target a route must end for it to count as heading
// there at all. Under this he is shut in and the route explores instead.
constexpr float kProgressWanted = 4.0f;
// And an exploring route is only worth walking if it goes somewhere.
constexpr float kExploreLeast = 12.0f;
// How near somewhere already explored a new exploring route may end.
constexpr float kExploredKeepOut = 25.0f;
// How far below his feet a floor still counts as the one he is on.
constexpr float kHangingReach = 4.0f;
// How much a cell that carries on the way the last one went is worth over
// one across it: half again at dead ahead.
constexpr float kCarryOn = 0.5f;
// And how far off that way a cell may be at all: a right angle, no more,
// while anywhere ahead is left to walk to.
constexpr float kMustCarryOn = 0.0f;
// A pulled leg no longer than this, so the walker replans on a scale it can
// see; and the squeeze out of whatever the start is painted inside.
constexpr float kMaxLeg = 60.0f;
constexpr float kSqueeze = 1.0f;
// How near a pickup counts as stepping on it. Not the pickup's own reach,
// which is a metre and a half: a server watches a range of its own round
// the point, and the dialog for the route map at the station opened with
// the character three metres nine from the icon. Four and a half covers the
// ranges servers use, and a street is wide enough to go round.
constexpr float kPickupDisc = 4.5f;
// A spot he got stuck at is painted this wide, unless he is still standing
// at it or it is where he is going.
constexpr float kStuckDisc = 1.0f;
constexpr float kStuckKeepOut = 1.5f;
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

struct Field::Work {
  enum class Phase { kLayout, kGround, kPaint, kClearance, kSearch, kPull, kDone };
  Phase phase = Phase::kLayout;
  Vec3  from, to;
  unsigned long long began_ms = 0;
  Grid  grid;
  float ref_z = 0;

  // Reading the ground: a flood outward from where he stands, a batch a
  // step. The queue holds the settled cells whose neighbours are still to
  // be read; `tries` counts the reads spent on a cell that has not settled,
  // so a hilltop reached from the steep side can still be reached from the
  // gentle one without the reading going round for ever.
  bool ground_seeded = false;
  int  stride = kGroundStride;   // cells between ground readings
  std::vector<int> queue;
  std::size_t queue_at = 0;
  std::vector<std::uint8_t> tries;

  // Painting, a tile a step.
  std::vector<Vec3> tile_centres;
  std::size_t tile_i = 0;
  std::vector<game::col::Body> bodies;

  int start = -1, goal = -1, end = -1;
  bool exploring = false;
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
      // Room to go round things, but no more than the errand can use: sixty
      // metres of margin either side of an eight-metre walk across a ward
      // makes a box a hundred and thirty metres across to cross a room, and
      // a box that big has to be read coarsely.
      const float straight_line = Away(w.from, w.to);
      const float margin =
          std::min(kRoundStart, std::max(kLeastRound, straight_line));
      const float roomx0 = std::min(w.from.x, w.to.x) - margin;
      const float roomx1 = std::max(w.from.x, w.to.x) + margin;
      const float roomy0 = std::min(w.from.y, w.to.y) - margin;
      const float roomy1 = std::max(w.from.y, w.to.y) + margin;
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
      w.stride = (W <= kCloseSide && H <= kCloseSide) ? kCloseStride : kGroundStride;
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
      // Pickups and the checkpoint are not floor. A pickup is a thing a
      // server puts on the ground to be walked into on purpose, and a route
      // that crosses one by accident opens whatever it opens - a dialog
      // nobody asked for, a shop, a teleport - and the dialog then takes the
      // keyboard and the walk with it. He stood twenty-five metres into a
      // journey with a dialog up for that reason. So they are painted solid,
      // except the one he was sent to and the one he is standing on.
      for (const samp::Pickup& pickup : samp::PickupsNear(w.from, reach, 128)) {
        if (Away(pickup.at, w.to) <= kPickupDisc + 0.5f) continue;
        if (Away(pickup.at, w.from) <= kSqueeze) continue;
        w.bodies.push_back(game::col::Body{pickup.at.x, pickup.at.y, pickup.at.z,
                                           kPickupDisc});
      }
      // Where he got stuck lately - against a gate the paint slipped
      // through, a car that has since moved on. The walker remembers them
      // and the graph plans round them; the field did not, and so planned
      // the same route through the same spot every time he handed back.
      for (const Vec3& spot : RememberedObstacles()) {
        if (Away(spot, w.from) <= kStuckKeepOut || Away(spot, w.to) <= kStuckKeepOut) continue;
        w.bodies.push_back(game::col::Body{spot.x, spot.y, spot.z, kStuckDisc});
      }
      {
        const samp::Checkpoint cp = samp::CheckpointNow(w.from);
        if (cp.shown && Away(cp.at, w.to) > cp.size + 1.0f && Away(cp.at, w.from) > kSqueeze)
          w.bodies.push_back(game::col::Body{cp.at.x, cp.at.y, cp.at.z,
                                             std::max(cp.size + 2.0f, kPickupDisc)});
      }
      w.phase = Phase::kGround;
      return false;
    }

    case Phase::kGround: {
      // The ground is flooded outward from the cell he is standing on, not
      // read square by square from his own height. Each cell is read from
      // the height of a settled neighbour a metre away and kept when it is
      // within a stride's rise of it, so the reading follows the floor he
      // is on wherever it goes - up a ramp, down a slipway, round a corner
      // - and stops where that floor stops.
      //
      // Reading everything from his own height instead is what left him at
      // the bottom of the canals in the middle of town with half the field
      // unknown: the canal floor is nine metres below the street, every
      // reading up there was refused for being too far from his feet, and
      // the search - which treats unknown as solid - had nowhere to go but
      // fourteen metres along the bottom. He replanned that same fourteen
      // metres every two seconds.
      if (!w.ground_seeded) {
        w.ground_seeded = true;
        w.tries.assign(static_cast<std::size_t>(g.W) * g.H, 0);
        int sx = 0, sy = 0;
        if (!g.cell_of(w.from, &sx, &sy))
          return finish("the start is outside the field");
        sx -= sx % w.stride;
        sy -= sy % w.stride;
        const int seed = g.index(sx, sy);
        const Vec3 c = g.centre(seed);
        float found = 0;
        ++result_.ground_reads;
        // The floor that is really under him. His feet are not it while he
        // is hanging off a ledge by his hands, and a flood seeded two
        // metres up refuses everything round it.
        float under = w.ref_z;
        if (game::col::GroundBelow(w.from.x, w.from.y, w.from.z + 1.0f, &under, true) &&
            under < w.ref_z + 0.5f && under > w.ref_z - kHangingReach) {
          w.ref_z = under;
          result_.ref_z = under;
        }
        // Where he is standing is ground whatever a read says: it is the
        // one square in the world that is known to hold him up.
        g.ground[seed] =
            (game::col::GroundBelow(c.x, c.y, w.ref_z + 4.0f, &found, false) &&
             std::fabs(found - w.ref_z) < kSameLevel)
                ? found
                : w.ref_z;
        g.known[seed] = 1;
        w.queue.push_back(seed);
      }
      int reads = 0;
      const int dx[4] = {w.stride, -w.stride, 0, 0};
      const int dy[4] = {0, 0, w.stride, -w.stride};
      while (w.queue_at < w.queue.size() && reads < kReadsPerStep) {
        const int at = w.queue[w.queue_at++];
        const int ix = at % g.W, iy = at / g.W;
        const float from_z = g.ground[at];
        for (int d = 0; d < 4; ++d) {
          const int nx = ix + dx[d], ny = iy + dy[d];
          if (!g.inside(nx, ny)) continue;
          const int next = g.index(nx, ny);
          if (g.known[next] == 1) continue;
          if (w.tries[next] >= kGroundTries) continue;
          ++w.tries[next];
          const Vec3 c = g.centre(next);
          float found = 0;
          ++reads;
          ++result_.ground_reads;
          // Read from the game's memory directly rather than through the
          // guarded call, which spends a slot per read out of a small
          // reserve and then answers "no ground" - forty per cent of a
          // street came back unknown that way.
          if (FloorNear(c.x, c.y, from_z, kStepChain, &found)) {
            g.known[next] = 1;
            g.ground[next] = found;
            w.queue.push_back(next);
          }
        }
      }
      if (w.queue_at >= w.queue.size()) {
        // Whatever the flood never reached is not ground he can walk on.
        // Only the cells it actually read are settled either way: the ones
        // between them are still nought, and the clearance phase gives each
        // of those the reading it belongs to. Marking every cell here
        // instead left fifteen cells in sixteen saying "no ground", and a
        // field of eighty-five per cent unknown is a field of walls.
        for (int iy = 0; iy < g.H; iy += w.stride)
          for (int ix = 0; ix < g.W; ix += w.stride) {
            const int at = g.index(ix, iy);
            if (g.known[at] != 1) g.known[at] = 2;
          }
        result_.settled = static_cast<int>(w.queue.size());
        w.phase = Phase::kPaint;
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
          for (int iy = cy - reach; iy < cy + reach; iy += w.stride)
            for (int ix = cx - reach; ix < cx + reach; ix += w.stride) {
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
      // Vehicles too: a car across the pavement is a wall until it leaves,
      // and the journey replans as it goes, so a snapshot is enough.
      if (!game::col::PaintFootprint(c.x, c.y, floor, kTileRadius, kCell, kBandLow,
                                     kBandHigh, kBodyRadius, {}, w.bodies, &fp, &floors,
                                     true))
        return false;
      ++result_.tiles;
      if (fp.starved) ++result_.starved;
      result_.faulted += fp.faulted;
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
      SmoothBetweenReadings(&g, w.stride);
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
      if (!w.searcher.Step(kExpandPerStep)) return false;
      // Where the search reached the target, or got meaningfully nearer to
      // it, that is the way. Where it did not - he is shut in somewhere and
      // the nearest point of his pen to the target is where he already
      // stands - the route goes to the far end of the pen instead. That is
      // how anybody gets out of a canal: walk along it until the way out
      // comes into view, which for the field means until the next box,
      // drawn from where this route ends, holds the ramp.
      w.end = w.searcher.end();
      if (!w.searcher.reached_goal()) {
        const float from_start = Away(w.from, w.to);
        const float gained = from_start - w.searcher.nearest_away();
        if (gained < kProgressWanted) {
          // The far end, but not the one he came from. Every cell the
          // search reached is scored by what it cost to walk to - the
          // further the better - with a cell that carries on the way the
          // last exploring route went worth half again as much, and one
          // within a stone's throw of somewhere already explored worth
          // nothing. Without that he walked to one end of the canal, then
          // the other, then the first again, for as long as anybody let him.
          const std::vector<Vec3> explored = ExploredPlaces();
          const Vec3 way = ExploringWay();
          const bool have_way = way.x != 0 || way.y != 0;
          int best = -1;
          float best_score = 0;
          for (int at = 0; at < g.W * g.H; ++at) {
            const float cost = w.searcher.cost(at);
            if (cost <= 0) continue;
            const Vec3 c = g.centre(at);
            const float from_here = Away(c, w.from);
            if (from_here < kExploreLeast) continue;
            bool been = false;
            for (const Vec3& was : explored)
              if (Away(c, was) <= kExploredKeepOut) { been = true; break; }
            if (been) continue;
            float score = cost;
            if (have_way) {
              const float along = ((c.x - w.from.x) * way.x +
                                   (c.y - w.from.y) * way.y) / from_here;
              // Not backwards at all, while there is anywhere ahead. A
              // hundred and forty metres back up the canal costs more to
              // walk than thirty metres on, so scoring by cost alone chose
              // it and he turned round after covering a hundred and
              // forty-five metres of new ground.
              if (along < kMustCarryOn) continue;
              score *= 1.0f + kCarryOn * along;
            }
            if (score > best_score) {
              best_score = score;
              best = at;
            }
          }
          // Nowhere ahead at all: he has walked that way to its end, and
          // turning round is now the only thing left to try.
          if (best < 0)
            for (int at = 0; at < g.W * g.H; ++at) {
              const float cost = w.searcher.cost(at);
              if (cost <= best_score || cost <= 0) continue;
              const Vec3 c = g.centre(at);
              if (Away(c, w.from) < kExploreLeast) continue;
              best_score = cost;
              best = at;
            }
          if (best >= 0 && Away(g.centre(best), w.from) >= kExploreLeast) {
            w.end = best;
            w.exploring = true;
            result_.exploring = true;
          }
        }
      }
      w.phase = Phase::kPull;
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
      const std::vector<int> cells = w.searcher.CellsTo(w.end);
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
                    "collision field %dx%d at %.1f m, floor every %.1f m: %d tiles, %d ground reads, "
                    "%d%% solid, %d%% unknown, %d ledge cells, %d starved, %d faulted, "
                    "%d cells of floor, route %.0f m in %d legs%s",
                    g.W, g.H, kCell, w.stride * kCell, result_.tiles, result_.ground_reads,
                    static_cast<int>(100.0f * result_.blocked / std::max(1, g.W * g.H)),
                    static_cast<int>(100.0f * result_.unknown / std::max(1, g.W * g.H)),
                    result_.ledges, result_.starved, result_.faulted, result_.settled,
                    result_.length_m, static_cast<int>(result_.points.size()) - 1,
                    result_.reaches_target ? "" : " - ends short of the target");
      result_.note = note;
      if (result_.exploring)
        result_.note += " - shut in, walking to the far end of what he can reach";
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
