#include "nav/planner.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "log.hpp"
#include "nav/field.hpp"

namespace gtabot::nav {
namespace {

// Where a ped's origin sits above the ground he stands on.
constexpr float kPedOrigin = 1.0f;
// How far below a point the ground may be before the point is in the air,
// and how far above before the point is underground.
constexpr float kMaxGroundBelow = 3.0f;
constexpr float kMaxGroundAbove = 1.5f;
// Where the ground probe starts, above the ped's origin. With objects
// counted, whatever the line meets first is the ground: 2.1 m above the
// last ground finds a ledge he can jump-climb and not the awning, the bus
// shelter roof or the tree over the pavement.
constexpr float kProbeAbove = 1.1f;
// Room above the ground for a person: from the knee to over the head.
constexpr float kHeadroomFrom = 0.3f;
constexpr float kHeadroomTo   = 1.9f;
// A person climbs a step and takes a drop without thinking. Beyond that he
// can still get up a ledge with a jump, and down a fair height by simply
// going over the edge - what he cannot do is fall further than he survives
// or scale a wall taller than himself.
constexpr float kSampleStep   = 1.0f;
constexpr float kMaxClimb     = 1.0f;
constexpr float kMaxJumpClimb = 2.0f;
constexpr float kMaxDrop      = 1.6f;
constexpr float kMaxJumpDrop  = 4.5f;
// Heights the line between samples is tested at. Knee and chest miss the
// one thing that stops a character dead on any road: a boom gate, a rail, a
// chain, all at about a metre. The waist catches those. Anything that
// blocks the lower lines but not the one at head height is low enough to
// jump - the way a player does.
constexpr float kKnee  = 0.5f;
constexpr float kWaist = 0.95f;
constexpr float kChest = 1.35f;
constexpr float kHead  = 1.9f;
// What each costs, in metres of walking, so a route without them wins when
// there is one and one with them wins over a long way round.
constexpr float kJumpPenalty        = 6.0f;
constexpr float kClimbPenalty       = 10.0f;
constexpr float kDropPenaltyBase    = 4.0f;
constexpr float kDropPenaltyPerMetre = 2.0f;

// The goal is usually a pair of coordinates somebody typed, at whatever
// height the character happens to be. Its ground is looked for from well
// above, so a target down the hill from him is not "in the air".
constexpr float kGoalProbeUp = 60.0f;

// Joining the route to the graph: how far to look for a node, and how many
// to try before giving up on that end.
constexpr float kJoinRadius = 70.0f;
constexpr std::size_t kJoinCandidates = 10;
// A* over the ped nodes stops here; the loaded graph is a few square
// kilometres and a route that needs more is not one he can walk yet.
constexpr int kMaxExpansions = 6000;
constexpr int kAStarBatch    = 400;

// Bridges: edges the graph does not have but a person does.
//
// The ped nodes are linked along each pavement and across roads only at
// crossings, so the shortest route between two pavements facing each other
// is down to the lights and back. A person crosses where he stands. So
// while searching, every ped node also reaches the ped nodes near it that
// it is not linked to - the far pavement, the fragment the network forgot -
// at a premium, and only past a first look along the line at chest height,
// which is one call and turns most of a city's buildings away at once.
// The leg is then walked properly like any other before it is trusted, and
// an edge that fails that is banned and the search run again without it.
constexpr float       kBridgeRadius     = 22.0f;
constexpr std::size_t kBridgeNeighbours = 6;
constexpr float       kBridgeCost       = 1.3f;
constexpr int         kBridgeLooks      = 2500;
constexpr int         kMaxReplans       = 4;

// Pulling the route tight.
//
// The graph is there to get around things, not to be followed. Left alone it
// produces exactly what it is - the pavement network the game's pedestrians
// walk - so a route to the far side of a street goes down to the crossing and
// back up, in big obedient loops, when a person would simply cross the road.
//
// So the route is pulled as tight as the world actually allows: from each
// point, the furthest one still reachable in a straight line wins, searched
// from the far end down so the longest legal shortcut is the one taken. What
// makes that safe is that the shortcut is not assumed - it is walked in
// sampling, metre by metre, ground and clearance the whole way, exactly as
// the direct line was. A road is walkable ground, so cutting across one is
// allowed; a building is not, so cutting through one is not.
//
// The lookahead has to cover the whole of a loop, or the loop stays: down
// to the lights and back can be forty nodes.
constexpr int   kSmoothLookahead = 48;
constexpr float kSmoothMaxLeg    = 220.0f;
constexpr int   kSmoothPasses    = 2;
// Shortcuts shorter than this are walked properly straight away; the
// one-line quick look is for the long ones, where it saves hundreds of
// calls. A short line that fails is then known to fail at a point, and
// that point can be gone round.
constexpr float kQuickLineFrom   = 60.0f;
// How far to the side of one thin thing - a tree, a post, a bench end -
// the bent line goes.
constexpr float kBendAside       = 3.0f;
constexpr int   kRejectedLogged  = 8;

// The search over open ground, for when the pavements do not reach.
//
// A grid of cells around the straight line, each asked of the game: is
// there ground here, can a person step from the cell before to this one.
// Eight neighbours, so the result zigzags, which the tightening then takes
// out. Bounded in cells and kept to a corridor about the line, because it
// costs a couple of dozen calls a cell and the point is to get out of a
// corner, not to survey the county.
constexpr float kLatticeCellMin     = 2.5f;
constexpr float kLatticeCellMax     = 4.0f;
constexpr float kLatticeCorridorMin = 50.0f;
constexpr int   kLatticeMaxCells    = 6000;
constexpr int   kLatticeBatch       = 6;
// A cell edge is first judged by one look between the two centres. When
// that says no - too steep, something in the way - the edge is walked
// properly, a metre at a time, because a staircase is exactly a slope too
// steep for one look and fine on foot. Bounded, being the expensive kind.
constexpr int   kLatticeFineLooks   = 6000;
// When the corridor turns out to be the problem - a fence longer than it is
// wide - one more go, wider and longer, before giving up.
constexpr float kLatticeWiderBy     = 2.4f;
constexpr int   kLatticeMaxCellsWide = 15000;
// Reaching a pavement from wherever he is - a roof, a freeway, a hilltop -
// is a search for the nearest one he can get to, not for the target. A cell
// this close to a ped node, with a walk to it, is the way onto the graph.
constexpr float kPavementReach = 3.5f;

// Feeling the way: the fan BestDirection casts - a sweep round the clock
// like a lidar's, each spoke walked in sampling as far as it goes, so a
// forest, a field or a yard full of small houses is read as what it is
// rather than guessed at. The leg then taken is long enough to be worth it.
constexpr int   kFanSpokes  = 24;
constexpr float kFanLength  = 14.0f;
constexpr float kFanMinReach = 2.5f;
constexpr float kFanMaxLeg  = 12.0f;
// Water deeper than this over the ground the game found is not ground.
constexpr float kDrowns = 0.5f;

// The whole plan, in calls into the game. Past this, legs are marked
// unverified rather than assumed. Calls are cheap - twenty thousand went by
// in eighty milliseconds - and a search for the way down off a freeway is
// tens of thousands of them; the frame budget is what keeps each frame
// short, this only stops a runaway.
constexpr int kCallBudget = 200000;
// And a wall clock, because the call count is a poor measure of how long
// the player has been standing there: a plan that has not been found in
// this many milliseconds of trying is handed back as "no route", and the
// journey feels its way instead.
// Twelve seconds, not six: the collision field ahead of the graph paints a
// street's worth of tiles and reads the ground under a metre grid, and on a
// three-hundred-metre journey that is a few seconds by itself. The fallbacks
// behind it must still have time to run when it does not reach.
constexpr unsigned long long kPlanDeadlineMs = 12000;
// A hillside: how much the ground may fall or rise over a quarter of a
// metre and still be a surface he walks (or slides) on rather than an edge.
constexpr float kSlopeSubStep    = 0.25f;
constexpr float kSlopeMaxDropSub = 0.9f;
constexpr float kSlopeMaxRiseSub = 0.3f;
constexpr float kSlopeMaxTotal   = 12.0f;
constexpr float kSteepPenaltyPerMetre = 1.5f;

// What the walker met and the plan did not know about. The points are put
// inside the thing itself, so the radius only has to cover its width, and
// not the ground in front of it.
constexpr float kObstacleRadius = 1.5f;
constexpr unsigned long long kObstacleMemoryMs = 120000;
constexpr std::size_t kMaxObstacles = 40;
// Where the memory does not apply: about the start, because he is standing
// there whatever was remembered, and about the goal, because a target next
// to where he once got stuck still has to be walked up to. If the thing is
// real he will meet it again there, and stop again, and say so.
constexpr float kExemptRadius = 4.0f;

int g_calls = 0;

struct Obstacle {
  Vec3 at;
  unsigned long long until_ms;
};
std::mutex            g_obstacle_mutex;
std::vector<Obstacle> g_obstacles;
// The ends of whatever is being planned right now. Game thread only.
Vec3 g_exempt[2];
int  g_exempt_count = 0;

void SetExempt(const Vec3* points, int count) {
  g_exempt_count = count > 2 ? 2 : count;
  for (int i = 0; i < g_exempt_count; ++i) g_exempt[i] = points[i];
}

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

float Distance3D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float dz = b.z - a.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string Metres(float value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f m", value);
  return buffer;
}

// What a walk costs, counting what it takes beyond walking.
float Penalty(const Verdict& v) {
  float penalty = v.jumps * kJumpPenalty + v.climbs * kClimbPenalty +
                  v.steep * kSteepPenaltyPerMetre;
  if (v.drop > kMaxDrop)
    penalty += kDropPenaltyBase + v.drop * kDropPenaltyPerMetre;
  return penalty;
}

bool GroundAt(const Vec3& p, float* ground);

// Whether the ground between two samples a metre apart, one much higher
// than the other, is a continuous slope rather than an edge: looked at every
// quarter metre, no single step may be more than a scramble up or a slide
// down. Three calls, only where the one-metre step already failed.
bool ContinuousSlope(const Vec3& from, float ground_from, const Vec3& to, float ground_to) {
  const float total = ground_to - ground_from;
  if (std::fabs(total) > kSlopeMaxTotal) return false;
  float previous = ground_from;
  for (int i = 1; i <= 3; ++i) {
    const float t = i * kSlopeSubStep;
    const Vec3 at{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                  previous + kPedOrigin};
    float ground = 0;
    if (!GroundAt(at, &ground)) return false;
    const float change = ground - previous;
    if (change > kSlopeMaxRiseSub || -change > kSlopeMaxDropSub) return false;
    previous = ground;
  }
  const float last = ground_to - previous;
  return last <= kSlopeMaxRiseSub && -last <= kSlopeMaxDropSub;
}
float Cost(const Verdict& v) { return v.metres + Penalty(v); }

bool NearRememberedObstacle(const Vec3& p) {
  for (int i = 0; i < g_exempt_count; ++i)
    if (Distance2D(p, g_exempt[i]) <= kExemptRadius) return false;
  std::lock_guard<std::mutex> lock(g_obstacle_mutex);
  const unsigned long long now = GetTickCount64();
  for (const Obstacle& o : g_obstacles) {
    if (now > o.until_ms) continue;
    if (Distance2D(p, o.at) <= kObstacleRadius &&
        std::fabs(p.z - o.at.z) < 3.0f)
      return true;
  }
  return false;
}

bool GroundAt(const Vec3& p, float* ground) {
  ++g_calls;
  return game::GroundBelow(Vec3{p.x, p.y, p.z + kProbeAbove}, ground);
}

// From high above, where anything the server has built is a roof between
// the question and its answer.
bool GroundAtNoObjects(const Vec3& p, float* ground) {
  ++g_calls;
  return game::GroundBelow(Vec3{p.x, p.y, p.z + kProbeAbove}, ground,
                           /*include_objects=*/false);
}

// Whether the ground found at (x, y) is under water. The ground call finds
// the bed of a lake as readily as a pavement; this is what tells them apart.
bool Drowns(float x, float y, float ground) {
  ++g_calls;
  float level = 0;
  return game::WaterLevel(Vec3{x, y, ground}, &level) && level > ground + kDrowns;
}

// "Nothing is in the way", separated from "nobody could tell us".
//
// A line-of-sight answer is not always to be had: it has to have passed its
// self-check and the calls have to be armed. Treating either as an obstacle
// is what once made every plan report "no headroom" about open air.
bool Clear(const Vec3& a, const Vec3& b) {
  if (!game::LineOfSightAvailable()) return true;
  ++g_calls;
  // Without vehicles. A route is planned once and walked afterwards, and by
  // then the car that was across the pavement has driven off - or a different
  // one has arrived. Cars are the walker's problem, at the moment it meets
  // one, not the planner's.
  return game::LineClear(a, b, /*include_vehicles=*/false);
}

Verdict StandableInner(const Vec3& p) {
  Verdict verdict;
  verdict.where = p;
  if (!game::CallsTrusted()) {
    verdict.why = "game calls are not verified yet";
    return verdict;
  }
  float ground = 0;
  if (!GroundAt(p, &ground)) {
    verdict.why = "no ground - not streamed in, or nothing there";
    return verdict;
  }
  verdict.ground_z = ground;
  if (Drowns(p.x, p.y, ground)) {
    verdict.why = "water";
    return verdict;
  }
  const float above = p.z - ground;
  if (above > kMaxGroundBelow) {
    verdict.why = "in the air, ground is " + Metres(above) + " below";
    return verdict;
  }
  // Headroom: a line straight up from just above the ground must be clear.
  if (!Clear(Vec3{p.x, p.y, ground + kHeadroomFrom},
             Vec3{p.x, p.y, ground + kHeadroomTo})) {
    verdict.why = "no headroom";
    return verdict;
  }
  verdict.ok = true;
  return verdict;
}

// A straight walk from a to b, sampled. `start_ground` is the ground under
// a when the caller already knows it; otherwise a is checked first.
Verdict WalkableFrom(const Vec3& a, float start_ground, const Vec3& b) {
  Verdict verdict;
  const float length = Distance2D(a, b);
  const int steps = length < kSampleStep
                        ? 1
                        : static_cast<int>(std::ceil(length / kSampleStep));
  float previous_ground = start_ground;
  Vec3  previous{a.x, a.y, start_ground};

  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    // Sampled at the height the ground was a step ago, so a slope or a
    // staircase is followed rather than measured from where it started.
    Vec3 sample{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                previous_ground + kPedOrigin};
    const float along = length * t;

    if (NearRememberedObstacle(sample)) {
      verdict.why    = "remembered obstacle at " + Metres(along);
      verdict.where  = sample;
      verdict.metres = along;
      return verdict;
    }
    float ground = 0;
    if (!GroundAt(sample, &ground)) {
      verdict.why   = "no ground at " + Metres(along);
      verdict.where = sample;
      verdict.metres = along;
      return verdict;
    }
    if (Drowns(sample.x, sample.y, ground)) {
      verdict.why   = "water at " + Metres(along);
      verdict.where = Vec3{sample.x, sample.y, ground + kPedOrigin};
      verdict.metres = along;
      return verdict;
    }
    const float change = ground - previous_ground;
    const Vec3 here{sample.x, sample.y, ground};
    bool steep = false;
    if (change > kMaxJumpClimb || -change > kMaxJumpDrop) {
      // Too much for a step or a jump - unless it is a hillside.
      if (!ContinuousSlope(Vec3{previous.x, previous.y, 0}, previous_ground,
                           Vec3{here.x, here.y, 0}, ground)) {
        verdict.why   = (change > 0 ? "climb of " : "drop of ") +
                        Metres(std::fabs(change)) + " at " + Metres(along);
        verdict.where = Vec3{sample.x, sample.y, ground + kPedOrigin};
        verdict.metres = along;
        return verdict;
      }
      steep = true;
      verdict.steep += std::fabs(change);
    }
    if (steep) {
      // Walked or slid; the lines between the samples would only find the
      // hill itself.
    } else if (change > kMaxClimb) {
      // A ledge he gets up with a jump. The lines between the samples would
      // only find its face, so it is not asked about.
      ++verdict.climbs;
    } else {
      if (-change > kMaxDrop && -change > verdict.drop) verdict.drop = -change;
      const bool low_clear =
          Clear(Vec3{previous.x, previous.y, previous.z + kKnee},
                Vec3{here.x, here.y, here.z + kKnee}) &&
          Clear(Vec3{previous.x, previous.y, previous.z + kWaist},
                Vec3{here.x, here.y, here.z + kWaist}) &&
          Clear(Vec3{previous.x, previous.y, previous.z + kChest},
                Vec3{here.x, here.y, here.z + kChest});
      if (!low_clear) {
        // Something in the way. Over his head too, and it is a wall;
        // otherwise it is a thing to jump.
        if (!Clear(Vec3{previous.x, previous.y, previous.z + kHead},
                   Vec3{here.x, here.y, here.z + kHead})) {
          verdict.why   = "blocked at " + Metres(along);
          verdict.where = Vec3{here.x, here.y, ground + kPedOrigin};
          verdict.metres = along;
          return verdict;
        }
        ++verdict.jumps;
        if (verdict.jumps <= 3) {
          char piece[80];
          std::snprintf(piece, sizeof(piece), " jump at %.0f m (ground %.2f -> %.2f)",
                        along, previous_ground, ground);
          verdict.detail += piece;
        }
      }
    }
    previous        = here;
    previous_ground = ground;
  }
  verdict.ok       = true;
  verdict.ground_z = previous_ground;
  verdict.where    = Vec3{b.x, b.y, previous_ground + kPedOrigin};
  verdict.metres   = length;
  return verdict;
}

Verdict WalkableInner(const Vec3& a, const Vec3& b) {
  Verdict start = StandableInner(a);
  if (!start.ok) {
    start.why = "start: " + start.why;
    return start;
  }
  return WalkableFrom(a, start.ground_z, b);
}

// A cheap first look at a long shortcut: one line at chest height, end to
// end. Most shortcuts through a building fail it, at one call instead of
// hundreds; the ones that pass are then walked properly.
bool QuickLine(const Vec3& a, const Vec3& b) {
  return Clear(Vec3{a.x, a.y, a.z - kPedOrigin + kChest},
               Vec3{b.x, b.y, b.z - kPedOrigin + kChest});
}

Vec3 Lifted(const game::PathNode& node) {
  return Vec3{node.pos.x, node.pos.y, node.pos.z + kPedOrigin};
}

std::uint32_t NodeKey(std::uint16_t area, std::uint16_t index) {
  return (static_cast<std::uint32_t>(area) << 16) | index;
}

struct Open {
  float         priority;
  std::uint32_t key;
  bool operator>(const Open& other) const { return priority > other.priority; }
};
using OpenQueue = std::priority_queue<Open, std::vector<Open>, std::greater<Open>>;

std::mutex g_debug_mutex;
DebugState g_debug;

}  // namespace

// ---- the job -------------------------------------------------------------

struct Planner::Job {
  enum class Stage { kEnds, kDirect, kField, kJoinStart, kJoinGoal, kAStar,
                     kLattice, kSmooth, kVerify, kDone };

  Vec3  from, to;   // as asked
  Vec3  a, b;       // lifted over the ground the game found
  Stage stage = Stage::kEnds;
  Plan  plan;
  unsigned long long began_ms = 0;
  int   calls_at_start = 0;
  int   steps = 0;
  std::string direct_why, field_why, graph_why, lattice_why;
  std::string source;

  // The walkability field: the world's collision painted onto a grid and
  // searched, before the pavement graph is consulted at all. It is what
  // makes a route keep off the walls; the graph and the open-ground search
  // stay behind it for when the field does not reach.
  nav::Field field;
  bool field_started = false;

  // The graph, copied once, and the search over it.
  bool        graph_loaded = false;
  game::Graph graph;
  std::vector<game::PathNode> start_candidates, goal_candidates;
  std::size_t    start_i = 0, goal_i = 0;
  std::string    last_join_why;
  game::PathNode start_node, goal_node;
  // The best join found so far on the end being tried: a plain walk to a
  // node wins at once; one that needs a jump or a drop is kept in case
  // nothing better turns up.
  bool           join_have = false;
  float          join_cost = 0;
  game::PathNode join_best;
  bool           astar_started = false;
  int            astar_expanded = 0;
  int            bridge_looks = 0;
  int            replans = 0;
  std::unordered_map<std::uint64_t, bool> bridge_seen;
  std::unordered_set<std::uint64_t>       banned;
  std::unordered_map<std::uint32_t, float>          astar_best;
  std::unordered_map<std::uint32_t, std::uint32_t>  astar_from;
  std::unordered_map<std::uint32_t, game::PathNode> astar_known;
  OpenQueue      astar_open;

  // The search over open ground.
  struct Cell {
    bool  evaluated = false;
    bool  ok        = false;
    float ground    = 0;
  };
  bool  lattice_started = false;
  int   lattice_attempt = 0;
  // When the pavements could not be joined from the start, the open ground
  // is searched for the nearest pavement he can reach, not for the target;
  // the graph takes it from there. What the search walked is kept as the
  // beginning of the route.
  bool  lattice_to_pavement = false;
  std::vector<Vec3> prefix;
  float cell = kLatticeCellMin;
  float corridor = kLatticeCorridorMin;
  int   lattice_expanded = 0;
  std::unordered_map<std::uint32_t, Cell>           cells;
  std::unordered_map<std::uint32_t, float>          lattice_best;
  std::unordered_map<std::uint32_t, std::uint32_t>  lattice_from;
  std::unordered_set<std::uint32_t>                 lattice_closed;
  // Directional: down a ledge is not up it. Negative means impassable.
  std::unordered_map<std::uint64_t, float>          edges;
  int       fine_looks = 0;
  OpenQueue lattice_open;

  // Pulling tight, then checking. `verified[k]` says the leg that ends at
  // point k has been walked in sampling and found fine.
  std::vector<Vec3> tight, pulled;
  std::vector<bool> tight_verified, pulled_verified;
  // Which graph node each point is, or 0, so a leg that fails its check can
  // be named as an edge and banned.
  std::vector<std::uint32_t> tight_keys, pulled_keys;
  bool        smoothing_started = false;
  int         pass = 0;
  std::size_t si = 0, sj = 0;
  std::size_t vk = 0;

  int CallsUsed() const { return g_calls - calls_at_start; }
  bool BudgetSpent() const {
    return CallsUsed() >= kCallBudget ||
           (began_ms != 0 && GetTickCount64() - began_ms >= kPlanDeadlineMs);
  }

  void Fail(const std::string& note) {
    plan.ok   = false;
    plan.note = note;
    Finish();
  }

  void Finish() {
    SetExempt(nullptr, 0);
    plan.game_calls = CallsUsed();
    plan.took_ms    = static_cast<int>(GetTickCount64() - began_ms);
    stage           = Stage::kDone;
    LOG_INFO("plan: finished in {} ms over {} steps, {} calls, {}", plan.took_ms,
             steps, plan.game_calls, plan.ok ? "ok - " + plan.note : plan.note);
  }

  std::string FailureNote() const {
    std::string note = "straight line " + direct_why;
    if (!field_why.empty()) note += "; field: " + field_why;
    if (!graph_why.empty()) note += "; pavements: " + graph_why;
    if (!lattice_why.empty()) note += "; open ground: " + lattice_why;
    return note;
  }

  // ---- stages, one unit of work each ----

  void Ends() {
    began_ms       = GetTickCount64();
    calls_at_start = g_calls;
    // The character is standing at the start, whatever the test says of it:
    // a roof, a ledge, a place with no headroom is still where he is. Only
    // the ground is looked up, to put the start at his feet.
    float ground = 0;
    a = GroundAt(from, &ground) ? Vec3{from.x, from.y, ground + kPedOrigin}
                                : from;
    // The goal: the ground under it, however far down - it is usually a
    // pair of coordinates at whatever height he happens to be - and only if
    // there is none, from well above, in case it is up a hill from him. That
    // order matters under an overpass: probing from sixty metres up first
    // would put the target on the road above.
    if (!GroundAt(to, &ground) &&
        !GroundAtNoObjects(Vec3{to.x, to.y, to.z + kGoalProbeUp - kMaxGroundAbove},
                  &ground)) {
      const float away = Distance2D(from, to);
      Fail("target: no ground there" +
           std::string(away > 250.0f ? " (" + Metres(away) +
                                           " away - beyond what the game "
                                           "has streamed in)"
                                     : ""));
      return;
    }
    b = Vec3{to.x, to.y, ground + kPedOrigin};
    const Vec3 ends[2] = {a, b};
    SetExempt(ends, 2);
    if (!Clear(Vec3{b.x, b.y, ground + kHeadroomFrom},
               Vec3{b.x, b.y, ground + kHeadroomTo})) {
      Fail("target: no headroom where the ground is");
      return;
    }
    stage = Stage::kDirect;
  }

  void Direct() {
    float start_ground = a.z - kPedOrigin;
    const Verdict direct = WalkableFrom(a, start_ground, b);
    if (direct.ok) {
      plan.waypoints = {a, b};
      Leg leg;
      leg.from = a;
      leg.to = b;
      leg.ok = true;
      leg.verified = true;
      plan.legs.push_back(leg);
      leg.jumps  = direct.jumps;
      leg.climbs = direct.climbs;
      leg.drop   = direct.drop;
      plan.jumps  = direct.jumps;
      plan.climbs = direct.climbs;
      plan.drops  = direct.drop > kMaxDrop ? 1 : 0;
      plan.length_m = Distance2D(a, b);
      plan.ok = true;
      plan.note = "straight line";
      if (plan.jumps > 0)  plan.note += ", " + std::to_string(plan.jumps) + " to jump";
      if (plan.climbs > 0) plan.note += ", " + std::to_string(plan.climbs) + " to climb";
      if (plan.drops > 0)  plan.note += ", a drop of " + Metres(direct.drop);
      Finish();
      return;
    }
    direct_why = direct.why;
    stage = Stage::kField;
  }

  void FieldStage() {
    if (!field_started) {
      field_started = true;
      field.Start(a, b);
    }
    if (!field.Step()) return;
    const nav::FieldResult& r = field.result();
    if (r.ok && r.reaches_target && r.points.size() >= 2) {
      plan.waypoints = r.points;
      plan.legs.clear();
      for (std::size_t i = 1; i < r.points.size(); ++i) {
        Leg leg;
        leg.from = r.points[i - 1];
        leg.to = r.points[i];
        leg.ok = true;
        leg.verified = true;
        plan.legs.push_back(leg);
      }
      plan.length_m = r.length_m;
      plan.ok = true;
      plan.note = r.note;
      source = "field";
      Finish();
      return;
    }
    field_why = r.note;
    stage = game::CachedPaths().valid ? Stage::kJoinStart : Stage::kLattice;
    if (stage == Stage::kLattice) graph_why = "the path graph is not available";
  }

  void JoinStart() {
    if (!graph_loaded) {
      graph_loaded = true;
      graph = game::SnapshotGraph();
      if (!graph.valid) {
        graph_why = "the path graph could not be read";
        stage = Stage::kLattice;
        return;
      }
      start_candidates = graph.PedNodesNear(a, kJoinRadius, kJoinCandidates);
      goal_candidates  = graph.PedNodesNear(b, kJoinRadius, kJoinCandidates);
    }
    if (start_i >= start_candidates.size() || BudgetSpent()) {
      if (join_have) {
        start_node = join_best;
        join_have = false;
        stage = Stage::kJoinGoal;
        return;
      }
      graph_why = start_candidates.empty()
                      ? "no ped node within " + Metres(kJoinRadius) + " of the start"
                      : "none of the " + std::to_string(start_candidates.size()) +
                            " nearest ped nodes is walkable from the start (last: " +
                            last_join_why + ")";
      // Then the open ground, to the nearest pavement he can actually reach.
      lattice_to_pavement = graph.valid;
      stage = Stage::kLattice;
      return;
    }
    const game::PathNode& node = start_candidates[start_i++];
    const Verdict verdict = WalkableFrom(a, a.z - kPedOrigin, Lifted(node));
    if (verdict.ok) {
      // Nearest is not best. The first walkable node used to win outright,
      // and getting on the graph at the nearest one can mean getting on it
      // behind you: measured on one journey, the first four legs were
      // fifty-seven metres of walking that ended thirty-four metres further
      // from where he was going than he started. What counts is what it
      // costs to get on plus how much journey is left once he is on, so
      // every candidate is weighed and the best kept.
      const float score = Cost(verdict) + Distance2D(node.pos, b);
      if (!join_have || score < join_cost) {
        join_have = true;
        join_cost = score;
        join_best = node;
      }
    } else {
      last_join_why = verdict.why;
    }
  }

  void JoinGoal() {
    if (goal_i >= goal_candidates.size() || BudgetSpent()) {
      if (join_have) {
        goal_node = join_best;
        join_have = false;
        stage = Stage::kAStar;
        return;
      }
      graph_why = goal_candidates.empty()
                      ? "no ped node within " + Metres(kJoinRadius) + " of the target"
                      : "none of the " + std::to_string(goal_candidates.size()) +
                            " nearest ped nodes reaches the target (last: " +
                            last_join_why + ")";
      stage = Stage::kLattice;
      return;
    }
    const game::PathNode& node = goal_candidates[goal_i++];
    // Walked the way he will walk it: from the node to the target.
    const Verdict verdict = WalkableInner(Lifted(node), b);
    if (verdict.ok && Penalty(verdict) == 0) {
      goal_node = node;
      join_have = false;
      stage = Stage::kAStar;
    } else if (verdict.ok) {
      if (!join_have || Cost(verdict) < join_cost) {
        join_have = true;
        join_cost = Cost(verdict);
        join_best = node;
      }
    } else {
      last_join_why = verdict.why;
    }
  }

  void AStar() {
    const std::uint32_t start_key = NodeKey(start_node.area, start_node.index);
    const std::uint32_t goal_key  = NodeKey(goal_node.area, goal_node.index);
    if (!astar_started) {
      astar_started = true;
      astar_best[start_key]  = 0;
      astar_known[start_key] = start_node;
      astar_known[goal_key]  = goal_node;
      astar_open.push({Distance3D(start_node.pos, goal_node.pos), start_key});
    }
    for (int n = 0; n < kAStarBatch; ++n) {
      if (astar_open.empty()) {
        graph_why = "no route through the loaded pavements after " +
                    std::to_string(astar_expanded) + " nodes";
        stage = Stage::kLattice;
        return;
      }
      const Open current = astar_open.top();
      astar_open.pop();
      if (current.key == goal_key) {
        RouteFromAStar(start_key, goal_key);
        return;
      }
      if (++astar_expanded > kMaxExpansions) {
        graph_why = "the pavement search gave up after " +
                    std::to_string(astar_expanded) + " nodes";
        stage = Stage::kLattice;
        return;
      }
      // Copied, not referenced: the map grows below, and a rehash would
      // leave a reference pointing at freed memory.
      const game::PathNode node = astar_known[current.key];
      const float here = astar_best[current.key];
      game::PathLink links[16];
      const int count = graph.Links(node, links, 16);
      const auto relax = [&](const game::PathLink& to, float premium) {
        const std::uint32_t key = NodeKey(to.area, to.index);
        if (key == current.key) return;
        if (banned.count(EdgeKey(current.key, key))) return;
        auto it = astar_known.find(key);
        if (it == astar_known.end()) {
          const game::PathNode* next = graph.Node(to.area, to.index);
          if (next == nullptr || !next->ped) return;  // a road, not a pavement
          it = astar_known.emplace(key, *next).first;
        }
        const float tentative = here + Distance3D(node.pos, it->second.pos) * premium;
        auto seen = astar_best.find(key);
        if (seen != astar_best.end() && tentative >= seen->second) return;
        astar_best[key] = tentative;
        astar_from[key] = current.key;
        astar_open.push({tentative + Distance3D(it->second.pos, goal_node.pos), key});
      };
      for (int i = 0; i < count; ++i) relax(links[i], 1.0f);

      // And the bridges: near ped nodes it is not linked to, past a look.
      if (bridge_looks < kBridgeLooks) {
        for (const game::PathLink& other_ref :
             graph.PedNodesAround(node.pos, kBridgeRadius, kBridgeNeighbours + 1)) {
          const std::uint32_t key = NodeKey(other_ref.area, other_ref.index);
          if (key == current.key) continue;
          bool linked = false;
          for (int i = 0; i < count && !linked; ++i)
            linked = NodeKey(links[i].area, links[i].index) == key;
          if (linked) continue;
          const std::uint64_t edge = EdgeKey(current.key, key);
          if (banned.count(edge)) continue;
          auto looked = bridge_seen.find(edge);
          bool open = false;
          if (looked != bridge_seen.end()) {
            open = looked->second;
          } else if (bridge_looks < kBridgeLooks) {
            const game::PathNode* other = graph.Node(other_ref.area, other_ref.index);
            if (other == nullptr) continue;
            ++bridge_looks;
            open = QuickLine(Lifted(node), Lifted(*other));
            bridge_seen[edge] = open;
          }
          if (open) relax(other_ref, kBridgeCost);
        }
      }
    }
  }

  void RouteFromAStar(std::uint32_t start_key, std::uint32_t goal_key) {
    std::vector<game::PathNode> reversed;
    std::uint32_t at = goal_key;
    while (true) {
      reversed.push_back(astar_known[at]);
      if (at == start_key) break;
      auto it = astar_from.find(at);
      if (it == astar_from.end()) break;
      at = it->second;
    }
    plan.graph_nodes = static_cast<int>(reversed.size());
    tight.clear();
    tight_verified.clear();
    tight_keys.clear();
    tight.push_back(a);
    tight_verified.push_back(false);
    tight_keys.push_back(0);
    // Whatever the open-ground search walked to reach the pavements comes
    // first, every step of it checked as it was searched.
    for (std::size_t i = 1; i < prefix.size(); ++i) {
      tight.push_back(prefix[i]);
      tight_verified.push_back(true);
      tight_keys.push_back(0);
    }
    for (auto it = reversed.rbegin(); it != reversed.rend(); ++it) {
      tight.push_back(Lifted(*it));
      // The leg into the first node was walked to join; the leg from the
      // last node to the target too. The ones between are the game's own
      // pavement links and the bridges, believed but not yet walked.
      tight_verified.push_back(it == reversed.rbegin());
      tight_keys.push_back(NodeKey(it->area, it->index));
    }
    tight.push_back(b);
    tight_verified.push_back(true);
    tight_keys.push_back(0);
    source = "via " + std::to_string(reversed.size()) + " ped nodes";
    if (!prefix.empty())
      source = "over open ground to a pavement, then " + source;
    smoothing_started = false;
    pass = 0;
    stage = Stage::kSmooth;
  }

  // Back to the search without the edge that failed, everything after the
  // search forgotten.
  void ReplanWithout(std::uint32_t from_key, std::uint32_t to_key) {
    banned.insert(EdgeKey(from_key, to_key));
    ++replans;
    astar_started = false;
    astar_expanded = 0;
    astar_best.clear();
    astar_from.clear();
    astar_known.clear();
    astar_open = OpenQueue();
    plan.legs.clear();
    plan.length_m = 0;
    plan.blocked_legs = 0;
    plan.jumps = plan.climbs = plan.drops = 0;
    stage = Stage::kAStar;
  }

  // ---- the lattice ----

  void ResetLattice() {
    lattice_started = false;
    cells.clear();
    lattice_best.clear();
    lattice_from.clear();
    lattice_closed.clear();
    edges.clear();
    fine_looks = 0;
    lattice_expanded = 0;
    lattice_open = OpenQueue();
  }

  // The cells from the origin to `key`, lifted to where a ped's origin is.
  std::vector<Vec3> CellsTo(std::uint32_t key) {
    std::vector<Vec3> reversed;
    std::uint32_t at = key;
    while (true) {
      int ix = 0, iy = 0;
      CellOf(at, &ix, &iy);
      const Cell& c = cells[at];
      reversed.push_back(Centre(ix, iy, c.ground + kPedOrigin));
      auto it = lattice_from.find(at);
      if (it == lattice_from.end()) break;
      at = it->second;
    }
    return std::vector<Vec3>(reversed.rbegin(), reversed.rend());
  }

  // Whether this cell is a way onto the pavements: a ped node close by, and
  // a walk to it. The origin does not count - that is where the join
  // already failed.
  bool CellReachesPavement(std::uint32_t key, const Vec3& centre_lifted) {
    if (!lattice_to_pavement || key == CellKey(0, 0)) return false;
    for (const game::PathLink& ref : graph.PedNodesAround(centre_lifted, kPavementReach, 3)) {
      const game::PathNode* node = graph.Node(ref.area, ref.index);
      if (node == nullptr || std::fabs(node->pos.z + kPedOrigin - centre_lifted.z) > 2.5f)
        continue;
      if (!WalkableFrom(centre_lifted, centre_lifted.z - kPedOrigin, Lifted(*node)).ok)
        continue;
      start_node = *node;
      prefix = CellsTo(key);
      plan.lattice_cells = lattice_expanded;
      LOG_INFO("plan: the open ground reaches a pavement after {} cells, {} m "
               "from the start", lattice_expanded,
               static_cast<int>(Distance2D(a, centre_lifted)));
      return true;
    }
    return false;
  }

  static std::uint32_t CellKey(int ix, int iy) {
    return (static_cast<std::uint32_t>(ix + 32768) << 16) |
           static_cast<std::uint32_t>(iy + 32768);
  }
  static void CellOf(std::uint32_t key, int* ix, int* iy) {
    *ix = static_cast<int>(key >> 16) - 32768;
    *iy = static_cast<int>(key & 0xFFFF) - 32768;
  }
  Vec3 Centre(int ix, int iy, float ground) const {
    return Vec3{a.x + ix * cell, a.y + iy * cell, ground};
  }
  static std::uint64_t EdgeKey(std::uint32_t p, std::uint32_t q) {
    if (p > q) std::swap(p, q);
    return (static_cast<std::uint64_t>(p) << 32) | q;
  }
  static std::uint64_t WayKey(std::uint32_t from, std::uint32_t to) {
    return (static_cast<std::uint64_t>(from) << 32) | to;
  }

  // What stepping from one cell to the next costs, or a negative number
  // when it cannot be done. One look first; the proper walk only when the
  // look says no, since that is where staircases, kerbs with rails and low
  // walls all live.
  float EdgeCost(const Vec3& centre, float here_ground, const Vec3& there,
                 float there_ground, float step) {
    const float change = there_ground - here_ground;
    if (change > kMaxJumpClimb || -change > kMaxJumpDrop) return -1.0f;
    const bool coarse_ok =
        change <= kMaxClimb && -change <= kMaxDrop &&
        Clear(Vec3{centre.x, centre.y, centre.z + kKnee},
              Vec3{there.x, there.y, there.z + kKnee}) &&
        Clear(Vec3{centre.x, centre.y, centre.z + kWaist},
              Vec3{there.x, there.y, there.z + kWaist}) &&
        Clear(Vec3{centre.x, centre.y, centre.z + kChest},
              Vec3{there.x, there.y, there.z + kChest});
    if (coarse_ok) return step;
    if (fine_looks >= kLatticeFineLooks) return -1.0f;
    ++fine_looks;
    const Verdict fine = WalkableFrom(
        Vec3{centre.x, centre.y, here_ground + kPedOrigin}, here_ground,
        Vec3{there.x, there.y, there_ground + kPedOrigin});
    if (!fine.ok) return -1.0f;
    return step + Penalty(fine);
  }
  bool InsideCorridor(const Vec3& p) const {
    // Distance from the straight line a-b, with room past both ends.
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float cx = a.x + dx * t, cy = a.y + dy * t;
    const float off = std::sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy));
    return off <= corridor;
  }

  void Lattice() {
    if (!lattice_started) {
      lattice_started = true;
      const float distance = Distance2D(a, b);
      cell = distance / 40.0f;
      cell = cell < kLatticeCellMin ? kLatticeCellMin
             : cell > kLatticeCellMax ? kLatticeCellMax : cell;
      corridor = std::max(kLatticeCorridorMin, distance * 0.6f);
      if (lattice_to_pavement) corridor = std::max(corridor, 120.0f);
      if (lattice_attempt > 0) corridor *= kLatticeWiderBy;
      Cell& origin = cells[CellKey(0, 0)];
      origin.evaluated = true;
      origin.ok        = true;
      origin.ground    = a.z - kPedOrigin;
      lattice_best[CellKey(0, 0)] = 0;
      lattice_open.push({distance, CellKey(0, 0)});
    }
    for (int n = 0; n < kLatticeBatch; ++n) {
      const int cap = lattice_attempt > 0 ? kLatticeMaxCellsWide : kLatticeMaxCells;
      const bool exhausted = lattice_open.empty();
      const bool capped = lattice_expanded >= cap || BudgetSpent();
      if (exhausted || capped) {
        if (lattice_to_pavement) {
          // No pavement to be reached that way; the target itself, then.
          LOG_INFO("plan: no pavement reachable over the open ground ({} cells) "
                   "- searching for the target itself", lattice_expanded);
          lattice_to_pavement = false;
          lattice_attempt = 0;
          ResetLattice();
          return;
        }
        if (exhausted && lattice_attempt == 0) {
          // The corridor was the limit, not the world. Once more, wider.
          LOG_INFO("plan: no way across the open ground within {} of the "
                   "line after {} cells - looking wider", Metres(corridor),
                   lattice_expanded);
          ++lattice_attempt;
          ResetLattice();
          return;
        }
        lattice_why = exhausted
            ? "no way across the open ground within " + Metres(corridor) +
                  " of the line (" + std::to_string(lattice_expanded) + " cells)"
            : "gave up after " + std::to_string(lattice_expanded) + " cells";
        Fail(FailureNote());
        return;
      }
      const Open current = lattice_open.top();
      lattice_open.pop();
      if (lattice_closed.count(current.key)) continue;
      lattice_closed.insert(current.key);
      ++lattice_expanded;

      int ix = 0, iy = 0;
      CellOf(current.key, &ix, &iy);
      const Cell here = cells[current.key];
      const Vec3 centre = Centre(ix, iy, here.ground);
      if (lattice_to_pavement) {
        if (CellReachesPavement(current.key,
                                Vec3{centre.x, centre.y, here.ground + kPedOrigin})) {
          stage = Stage::kJoinGoal;
          return;
        }
      } else if (Distance2D(centre, b) <= cell * 0.8f &&
                 std::fabs((here.ground + kPedOrigin) - b.z) <= 2.0f) {
        // There, in the plane and in height both: a cell on the platform
        // above the target is not the target.
        RouteFromLattice(current.key);
        return;
      }
      const float cost_here = lattice_best[current.key];
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          const int nx = ix + dx, ny = iy + dy;
          const std::uint32_t key = CellKey(nx, ny);
          if (lattice_closed.count(key)) continue;
          Cell& next = cells[key];
          if (!next.evaluated) {
            next.evaluated = true;
            const Vec3 probe = Centre(nx, ny, here.ground + kPedOrigin);
            float ground = 0;
            next.ok = InsideCorridor(probe) && !NearRememberedObstacle(probe) &&
                      GroundAt(probe, &ground) && !Drowns(probe.x, probe.y, ground);
            next.ground = ground;
          }
          if (!next.ok) continue;

          const float step = cell * ((dx != 0 && dy != 0) ? 1.41421f : 1.0f);
          const std::uint64_t way = WayKey(current.key, key);
          float edge_cost = 0;
          auto known = edges.find(way);
          if (known != edges.end()) {
            edge_cost = known->second;
          } else {
            const Vec3 there = Centre(nx, ny, next.ground);
            edge_cost = EdgeCost(centre, here.ground, there, next.ground, step);
            edges[way] = edge_cost;
          }
          if (edge_cost < 0) continue;

          const float tentative = cost_here + edge_cost;
          auto seen = lattice_best.find(key);
          if (seen != lattice_best.end() && tentative >= seen->second) continue;
          lattice_best[key] = tentative;
          lattice_from[key] = current.key;
          const Vec3 there = Centre(nx, ny, next.ground);
          lattice_open.push({tentative + Distance2D(there, b), key});
        }
      }
    }
  }

  void RouteFromLattice(std::uint32_t goal_key) {
    std::vector<Vec3> reversed;
    std::uint32_t at = goal_key;
    while (true) {
      int ix = 0, iy = 0;
      CellOf(at, &ix, &iy);
      const Cell& c = cells[at];
      reversed.push_back(Centre(ix, iy, c.ground + kPedOrigin));
      auto it = lattice_from.find(at);
      if (it == lattice_from.end()) break;
      at = it->second;
    }
    plan.lattice_cells = lattice_expanded;
    tight.clear();
    tight_verified.clear();
    tight.push_back(a);
    tight_verified.push_back(false);
    // The first cell is the start itself; every step between cells was
    // checked as it was searched.
    for (auto it = reversed.rbegin() + 1; it < reversed.rend(); ++it) {
      tight.push_back(*it);
      tight_verified.push_back(true);
    }
    tight.push_back(b);
    tight_verified.push_back(false);
    tight_keys.assign(tight.size(), 0);
    source = "over open ground, " + std::to_string(reversed.size()) + " cells";
    stage = Stage::kSmooth;
  }

  // ---- pulling tight ----

  std::size_t Furthest(std::size_t i) const {
    return std::min(tight.size() - 1,
                    i + 1 + static_cast<std::size_t>(kSmoothLookahead));
  }

  // Why the last leg is not a straight line to the target, for the log -
  // the route that goes round the block when the target is twenty metres
  // away is explained here or nowhere.
  int  rejected_logged = 0;
  bool budget_noted = false;
  void NoteRejected(std::size_t i, std::size_t j, const std::string& why) {
    const bool to_goal = j + 1 == tight.size();
    // Short ones too, on the first pass: a street of ten-metre legs that
    // will not merge is a fault somewhere, and this is where it shows.
    const bool short_one = pass == 0 && j >= i + 2 &&
                           Distance2D(tight[i], tight[j]) <= 40.0f;
    if ((!to_goal && !short_one) || rejected_logged >= kRejectedLogged) return;
    ++rejected_logged;
    LOG_INFO("plan: no straight line from ({:.0f},{:.0f}) to {}({:.0f},{:.0f}) "
             "({:.0f} m): {}", tight[i].x, tight[i].y, to_goal ? "the target " : "",
             tight[j].x, tight[j].y, Distance2D(tight[i], tight[j]), why);
  }

  // A straight line stopped by one thin thing - a tree, a lamp post, the
  // end of a bench - is walked with a bend: out to one side of the thing
  // and on to the far point. Two short legs a few metres longer than the
  // line, instead of the way round the block. A wall stops both sides.
  bool BendRound(std::size_t i, std::size_t j, const Verdict& straight, float around) {
    if (straight.why.compare(0, 7, "blocked") != 0) return false;
    const float length = Distance2D(tight[i], tight[j]);
    if (length < 4.0f || straight.metres < 2.0f || straight.metres > length - 2.0f)
      return false;
    if (BudgetSpent()) return false;
    const float dx = (tight[j].x - tight[i].x) / length;
    const float dy = (tight[j].y - tight[i].y) / length;
    bool  have = false;
    float best_cost = 0;
    Vec3  best_via{};
    for (int side = -1; side <= 1; side += 2) {
      Vec3 via{straight.where.x - dy * kBendAside * static_cast<float>(side),
               straight.where.y + dx * kBendAside * static_cast<float>(side),
               straight.where.z};
      const Verdict first = WalkableFrom(tight[i], tight[i].z - kPedOrigin, via);
      if (!first.ok) continue;
      via.z = first.ground_z + kPedOrigin;
      const Verdict second = WalkableFrom(via, first.ground_z, tight[j]);
      if (!second.ok) continue;
      const float cost = Cost(first) + Cost(second);
      if (!have || cost < best_cost) {
        have = true;
        best_cost = cost;
        best_via = via;
      }
    }
    if (!have || best_cost >= around) return false;
    LOG_INFO("plan: the line from ({:.0f},{:.0f}) to ({:.0f},{:.0f}) is {} - "
             "bending round it via ({:.0f},{:.0f}), {:.0f} m against {:.0f} m round",
             tight[i].x, tight[i].y, tight[j].x, tight[j].y, straight.why,
             best_via.x, best_via.y, best_cost, around);
    pulled.push_back(best_via);
    pulled_verified.push_back(true);
    pulled_keys.push_back(0);
    pulled.push_back(tight[j]);
    pulled_verified.push_back(true);
    pulled_keys.push_back(tight_keys[j]);
    si = j;
    sj = Furthest(si);
    return true;
  }

  void Smooth() {
    if (!smoothing_started) {
      smoothing_started = true;
      pulled.assign(1, tight.front());
      pulled_verified.assign(1, false);
      pulled_keys.assign(1, tight_keys.front());
      si = 0;
      sj = Furthest(si);
    }
    if (si + 1 >= tight.size()) {
      const bool changed = pulled.size() != tight.size();
      tight          = pulled;
      tight_verified = pulled_verified;
      tight_keys     = pulled_keys;
      ++pass;
      if (!changed || pass >= kSmoothPasses || tight.size() <= 2) {
        stage = Stage::kVerify;
        vk = 1;
        return;
      }
      pulled.assign(1, tight.front());
      pulled_verified.assign(1, false);
      pulled_keys.assign(1, tight_keys.front());
      si = 0;
      sj = Furthest(si);
      return;
    }
    if (sj <= si + 1) {
      // No shortcut from here; keep the next point as it was.
      pulled.push_back(tight[si + 1]);
      pulled_verified.push_back(tight_verified[si + 1]);
      pulled_keys.push_back(tight_keys[si + 1]);
      ++si;
      sj = Furthest(si);
      return;
    }
    // One candidate shortcut: from the far end down, so the longest legal
    // one is the one taken.
    const float straight_m = Distance2D(tight[si], tight[sj]);
    if (BudgetSpent() && !budget_noted) {
      budget_noted = true;
      LOG_WARN("plan: the call budget ran out before the smoothing - the route "
               "stays as the graph gave it ({} legs)", tight.size() - 1);
    }
    if (BudgetSpent() || straight_m > kSmoothMaxLeg ||
        (straight_m > kQuickLineFrom && !QuickLine(tight[si], tight[sj]))) {
      if (!BudgetSpent() && straight_m <= kSmoothMaxLeg)
        NoteRejected(si, sj, "a line at chest height is blocked");
      --sj;
      return;
    }
    // The way round it would replace, in metres walked.
    float around = 0;
    for (std::size_t k = si; k < sj; ++k)
      around += Distance2D(tight[k], tight[k + 1]);
    const Verdict shortcut = WalkableFrom(tight[si], tight[si].z - kPedOrigin, tight[sj]);
    if (shortcut.ok) {
      // Cheaper than the way round it replaces, counting what it costs
      // beyond walking - a straight line over a wall is not a shortcut.
      if (Cost(shortcut) < around) {
        pulled.push_back(tight[sj]);
        pulled_verified.push_back(true);
        pulled_keys.push_back(tight_keys[sj]);
        si = sj;
        sj = Furthest(si);
        return;
      }
      NoteRejected(si, sj, "walkable but dearer - " + Metres(Cost(shortcut)) +
                               " with " + std::to_string(shortcut.jumps) + " to jump, " +
                               std::to_string(shortcut.climbs) + " to climb, against " +
                               Metres(around) + " round" + shortcut.detail);
    } else if (BendRound(si, sj, shortcut, around)) {
      return;
    } else {
      NoteRejected(si, sj, shortcut.why);
    }
    --sj;
  }

  // ---- checking what was not walked yet, and finishing ----

  void Verify() {
    if (vk >= tight.size()) {
      Complete();
      return;
    }
    Leg leg;
    leg.from      = tight[vk - 1];
    leg.to        = tight[vk];
    leg.via_graph = vk != 1 && vk + 1 != tight.size();
    if (tight_verified[vk]) {
      leg.ok       = true;
      leg.verified = true;
    } else if (BudgetSpent()) {
      leg.ok       = true;
      leg.verified = false;
      leg.why      = "unverified - call budget spent";
    } else {
      const Verdict verdict =
          WalkableFrom(leg.from, leg.from.z - kPedOrigin, leg.to);
      leg.ok       = verdict.ok;
      leg.verified = true;
      leg.why      = verdict.why;
      leg.jumps    = verdict.jumps;
      leg.climbs   = verdict.climbs;
      leg.drop     = verdict.drop;
      plan.jumps  += verdict.jumps;
      plan.climbs += verdict.climbs;
      if (verdict.drop > kMaxDrop) ++plan.drops;
      if (!leg.ok && tight_keys[vk - 1] != 0 && tight_keys[vk] != 0 &&
          replans < kMaxReplans && CallsUsed() < kCallBudget / 2) {
        // An edge of the graph he cannot actually walk - a bridge through
        // something, or a pavement with something on it. Without it, again -
        // while half the budget is left: a search that leaves nothing for
        // the smoothing gives a route of a hundred short legs.
        LOG_INFO("plan: the graph edge ({:.0f},{:.0f}) -> ({:.0f},{:.0f}) is {} - "
                 "searching again without it (attempt {})", leg.from.x, leg.from.y,
                 leg.to.x, leg.to.y, verdict.why, replans + 2);
        ReplanWithout(tight_keys[vk - 1], tight_keys[vk]);
        return;
      }
    }
    plan.length_m += Distance2D(leg.from, leg.to);
    if (!leg.ok) ++plan.blocked_legs;
    plan.legs.push_back(std::move(leg));
    ++vk;
  }

  void Complete() {
    plan.waypoints = tight;
    // A blocked leg is reported, not fatal. Legs between the game's own ped
    // nodes are walkable by construction - its pedestrians walk them - while
    // the test applied to them counts a kerb, a lamppost and a bin as walls.
    // Throwing the route away for one of them is why nothing behind a wall
    // could ever be reached when the route around the wall was right there.
    plan.ok   = true;
    plan.note = source + ", pulled to " + std::to_string(tight.size() - 1) +
                " legs";
    if (plan.blocked_legs > 0)
      plan.note += ", " + std::to_string(plan.blocked_legs) +
                   " of them tight enough to need stepping around";
    if (plan.jumps > 0)  plan.note += ", " + std::to_string(plan.jumps) + " to jump";
    if (plan.climbs > 0) plan.note += ", " + std::to_string(plan.climbs) + " to climb";
    if (plan.drops > 0)  plan.note += ", " + std::to_string(plan.drops) + " to jump down";
    Finish();
  }

  void StepOnce() {
    ++steps;
    switch (stage) {
      case Stage::kEnds:      Ends(); break;
      case Stage::kDirect:    Direct(); break;
      case Stage::kField:     FieldStage(); break;
      case Stage::kJoinStart: JoinStart(); break;
      case Stage::kJoinGoal:  JoinGoal(); break;
      case Stage::kAStar:     AStar(); break;
      case Stage::kLattice:   Lattice(); break;
      case Stage::kSmooth:    Smooth(); break;
      case Stage::kVerify:    Verify(); break;
      case Stage::kDone:      break;
    }
  }
};

Planner::Planner() = default;
Planner::~Planner() = default;

void Planner::Start(const Vec3& from, const Vec3& to) {
  job_ = std::make_unique<Job>();
  job_->from = from;
  job_->to   = to;
  result_    = Plan{};
  LOG_INFO("plan: starting ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f})",
           from.x, from.y, from.z, to.x, to.y, to.z);
}

bool Planner::Step(int budget_ms) {
  if (!job_) return true;
  if (job_->stage == Job::Stage::kDone) return true;
  if (!game::CallsTrusted()) {
    job_->plan.ok = false;
    job_->Fail("game calls are not verified yet");
    result_ = job_->plan;
    return true;
  }
  const unsigned long long until = GetTickCount64() + static_cast<unsigned>(budget_ms);
  do {
    // Past the game-call ceiling every answer is "no ground"; better to
    // finish next frame than to plan against a world that is not there.
    if (game::CallSlotsLeft() < 400) return false;
    job_->StepOnce();
    if (job_->stage == Job::Stage::kDone) {
      result_ = job_->plan;
      return true;
    }
  } while (GetTickCount64() < until);
  return false;
}

void Planner::Cancel() {
  if (job_ && job_->stage != Job::Stage::kDone) SetExempt(nullptr, 0);
  job_.reset();
}

bool Planner::active() const {
  return job_ != nullptr && job_->stage != Job::Stage::kDone;
}

bool Planner::finished() const {
  return job_ != nullptr && job_->stage == Job::Stage::kDone;
}

const Plan& Planner::result() const { return result_; }

// ---- the one-shot answers ----------------------------------------------

Verdict Standable(const Vec3& p) {
  const int before = g_calls;
  Verdict verdict = StandableInner(p);
  verdict.calls = g_calls - before;
  return verdict;
}

float CostOf(const Verdict& v) { return Cost(v); }

Verdict Walkable(const Vec3& a, const Vec3& b) {
  const int before = g_calls;
  Verdict verdict = WalkableInner(a, b);
  verdict.calls = g_calls - before;
  return verdict;
}

Plan PlanPath(const Vec3& from, const Vec3& to) {
  // Whoever asked is waiting, so this takes bigger bites than the journey
  // does - and still stops, because a caller waiting on the game thread is
  // a frame nobody draws.
  constexpr int kBiteMs = 25;
  constexpr unsigned long long kPatienceMs = 500;
  Planner planner;
  planner.Start(from, to);
  const unsigned long long began = GetTickCount64();
  while (!planner.Step(kBiteMs)) {
    if (GetTickCount64() - began > kPatienceMs) {
      Plan plan;
      plan.note = "not finished within half a second - ask the journey "
                  "instead, it plans a few milliseconds at a time";
      LOG_WARN("plan: {}", plan.note);
      return plan;
    }
  }
  return planner.result();
}

// Where the fan has already sent him lately. Feeling the way along a ridge
// with the target off its edge, the two ways along the ridge score alike,
// and without a memory he would take them in turn forever.
struct Visited {
  Vec3 at;
  unsigned long long ms;
};
std::vector<Visited> g_visited;
constexpr unsigned long long kVisitedMemoryMs = 120000;
constexpr float kVisitedRadius = 6.0f;

bool BestDirection(const Vec3& here, const Vec3& target, Vec3* out,
                   std::string* why) {
  if (!game::CallsTrusted()) {
    *why = "game calls are not verified yet";
    return false;
  }
  const unsigned long long now_ms = GetTickCount64();
  g_visited.erase(std::remove_if(g_visited.begin(), g_visited.end(),
                                 [now_ms](const Visited& v) {
                                   return now_ms - v.ms > kVisitedMemoryMs;
                                 }),
                  g_visited.end());
  float ground = 0;
  if (!GroundAt(here, &ground)) ground = here.z - kPedOrigin;
  const Vec3 a{here.x, here.y, ground + kPedOrigin};
  const float toward = std::atan2(target.y - here.y, target.x - here.x);
  SetExempt(&a, 1);

  float best_score = 0;
  Vec3  best;
  bool  found = false;
  std::vector<Vec3> ends;
  std::vector<bool> ok;
  for (int i = 0; i < kFanSpokes; ++i) {
    // Around the clock starting from the target's own direction.
    const float angle = toward + static_cast<float>(i) * 6.2831853f / kFanSpokes;
    const Vec3 b{a.x + std::cos(angle) * kFanLength,
                 a.y + std::sin(angle) * kFanLength, a.z};
    const Verdict verdict = WalkableFrom(a, ground, b);
    // How far it got: all the way, or up to the last good sample.
    const float reach = verdict.ok ? kFanLength
                                   : std::max(0.0f, verdict.metres - kSampleStep);
    ends.push_back(verdict.ok ? verdict.where
                              : Vec3{a.x + std::cos(angle) * reach,
                                     a.y + std::sin(angle) * reach, a.z});
    ok.push_back(verdict.ok);
    if (reach < kFanMinReach) continue;
    // Distance gained, weighted toward the target; a way straight back is
    // still a way, at a quarter of the credit, for when nothing else is.
    // What the way costs beyond walking comes off - except a drop when the
    // target is down there, which is then the whole point.
    float cosine = std::cos(angle - toward);
    if (cosine < 0) cosine = 0;
    float penalty = Penalty(verdict);
    if (target.z < here.z - 1.5f && verdict.drop > kMaxDrop)
      penalty -= kDropPenaltyBase + verdict.drop * kDropPenaltyPerMetre;
    float score = reach * (0.25f + 0.75f * cosine) + (verdict.ok ? 1.0f : 0.0f) -
                  penalty * 0.5f;
    // Been there: a direction that leads back to somewhere recent is worth
    // a fraction of one that leads somewhere new.
    const Vec3 end_point{a.x + std::cos(angle) * reach * 0.85f,
                         a.y + std::sin(angle) * reach * 0.85f, a.z};
    for (const Visited& v : g_visited)
      if (Distance2D(v.at, end_point) < kVisitedRadius) { score *= 0.3f; break; }
    if (score <= best_score) continue;
    best_score = score;
    found = true;
    const float use = std::min(reach * 0.85f, kFanMaxLeg);
    best = Vec3{a.x + std::cos(angle) * use, a.y + std::sin(angle) * use,
                verdict.ok ? verdict.where.z : a.z};
  }
  SetDebugFan(std::move(ends), std::move(ok));
  SetExempt(nullptr, 0);
  if (!found) {
    *why = "no walkable direction within " + Metres(kFanLength);
    return false;
  }
  g_visited.push_back(Visited{here, now_ms});
  g_visited.push_back(Visited{best, now_ms});
  if (g_visited.size() > 40) g_visited.erase(g_visited.begin(), g_visited.begin() + 2);
  *out = best;
  return true;
}

void RememberObstacle(const Vec3& at, const char* what) {
  std::lock_guard<std::mutex> lock(g_obstacle_mutex);
  const unsigned long long now = GetTickCount64();
  g_obstacles.erase(std::remove_if(g_obstacles.begin(), g_obstacles.end(),
                                   [now](const Obstacle& o) {
                                     return now > o.until_ms;
                                   }),
                    g_obstacles.end());
  for (const Obstacle& o : g_obstacles)
    if (Distance2D(o.at, at) < kObstacleRadius * 0.5f) return;
  if (g_obstacles.size() >= kMaxObstacles) g_obstacles.erase(g_obstacles.begin());
  g_obstacles.push_back(Obstacle{at, now + kObstacleMemoryMs});
  LOG_INFO("nav: remembering {} at ({:.1f}, {:.1f}) - the next plan goes "
           "round it", what, at.x, at.y);
}

void ForgetObstacles() {
  std::lock_guard<std::mutex> lock(g_obstacle_mutex);
  g_obstacles.clear();
}

// ---- what the panel draws ------------------------------------------------

void SetDebugPlan(const Vec3& target, const Plan& plan) {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  g_debug.has_target = true;
  g_debug.target     = target;
  g_debug.plan       = plan;
}

void SetDebugFan(std::vector<Vec3> ends, std::vector<bool> ok) {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  g_debug.fan_ends = std::move(ends);
  g_debug.fan_ok   = std::move(ok);
}

void SetDebugNodes(std::vector<game::PathNode> nodes) {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  g_debug.nodes = std::move(nodes);
}

void SetDebugWhiskers(const Vec3& origin, std::vector<Vec3> ends,
                      std::vector<bool> clear) {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  g_debug.whisker_origin = origin;
  g_debug.whisker_ends   = std::move(ends);
  g_debug.whisker_clear  = std::move(clear);
}

void ClearDebug() {
  std::lock_guard<std::mutex> lock(g_debug_mutex);
  g_debug = DebugState{};
}

DebugState GetDebug() {
  DebugState out;
  {
    std::lock_guard<std::mutex> lock(g_debug_mutex);
    out = g_debug;
  }
  std::lock_guard<std::mutex> lock(g_obstacle_mutex);
  const unsigned long long now = GetTickCount64();
  for (const Obstacle& o : g_obstacles)
    if (now <= o.until_ms) out.obstacles.push_back(o.at);
  return out;
}

}  // namespace gtabot::nav
