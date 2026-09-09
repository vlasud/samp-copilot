#pragma once
//
// Decides where a character can stand, where he can walk, and how to get
// from here to there.
//
// Three questions, answered by asking the game rather than by reasoning
// about geometry:
//
//   Standable(p)   - is there ground under p, close enough to stand on, and
//                    room above it for a person?
//   Walkable(a, b) - can he walk a straight line from a to b? Sampled a metre
//                    at a time: ground the whole way, no step or drop bigger
//                    than a person takes, nothing solid at knee or chest
//                    height between one sample and the next.
//   a route        - straight if the straight line is walkable. Otherwise
//                    along the game's own ped nodes - the pavements its
//                    pedestrians use - joined to the ends by walkable legs.
//                    And when the pavements do not reach - the character is
//                    on a roof, a hillside, a building site, anywhere the
//                    game never expected a pedestrian - a search over the
//                    open ground itself, cell by cell, asking the game
//                    about each one. Either way the route is then pulled
//                    tight, so a person who would cross the road crosses
//                    the road instead of walking to the lights and back.
//
// A route is planned in steps, on the game thread, a few milliseconds a
// frame. A plan across a district is thousands of calls into the game and
// a search over thousands of nodes; done in one go that is a frame nobody
// draws, and done under a deadline short enough to draw it is a route
// nobody pulled tight - which is exactly the route that walks to the
// crossing and back. So the work is split into units small enough to fit
// beside a frame, and a caller steps it until it says it is finished.
//
// Positions are where a ped's origin sits, about a metre above his feet,
// which is how the game reports the player's own position. Points that come
// in at ground level are lifted; points that go out are snapped to the
// ground the game found.
//
#include <memory>
#include <string>
#include <vector>

#include "game/paths.hpp"
#include "game/world_query.hpp"

namespace gtabot::nav {

using game::Vec3;

struct Verdict {
  bool        ok = false;
  std::string why;         // empty when ok
  Vec3        where;       // the point that decided it
  float       ground_z = 0;
  float       metres   = 0;  // along the segment, for Walkable
  int         calls    = 0;  // into the game, to answer this
  // What walking it takes beyond walking: low things to jump over (a boom
  // gate, a low wall), ledges to climb (one to two metres), and the biggest
  // drop beyond a step (down to what a person survives). All allowed, all
  // costed, so a route with none is preferred when one exists.
  int         jumps  = 0;
  int         climbs = 0;
  float       drop   = 0;
  // Metres of hillside steeper than a step: walked and slid down, or
  // scrambled up, rather than stepped. Costed, not refused.
  float       steep  = 0;
  // The first few jumps, for the log: where, and the ground either side.
  std::string detail;
};

Verdict Standable(const Vec3& p);
Verdict Walkable(const Vec3& a, const Vec3& b);
// Metres plus what the jumps, climbs, drops and hillsides cost.
float CostOf(const Verdict& v);

struct Leg {
  Vec3        from, to;
  bool        ok        = false;
  bool        verified  = false;  // false when the call budget ran out first
  bool        via_graph = false;
  int         jumps  = 0;
  int         climbs = 0;
  float       drop   = 0;
  std::string why;
};

struct Plan {
  bool             ok = false;
  std::vector<Vec3> waypoints;   // from, ..., to
  std::vector<Leg>  legs;
  float            length_m    = 0;
  int              graph_nodes = 0;   // ped nodes the route passed through
  int              lattice_cells = 0; // cells the open-ground search visited
  // Legs the strict test called blocked. Not a failure: see the note in the
  // planner about why these are reported rather than fatal.
  int              blocked_legs = 0;
  int              jumps  = 0;        // over the whole route
  int              climbs = 0;
  int              drops  = 0;
  int              game_calls  = 0;
  int              took_ms     = 0;   // wall time, across however many steps
  std::string      note;
};

// One route being worked out. Game thread only.
class Planner {
 public:
  Planner();
  ~Planner();
  Planner(const Planner&) = delete;
  Planner& operator=(const Planner&) = delete;

  void Start(const Vec3& from, const Vec3& to);
  // Does at most about `budget_ms` of work, one unit past it at the worst,
  // and says whether the plan is finished. Safe to call when it is.
  bool Step(int budget_ms);
  void Cancel();

  bool        active() const;     // started and not yet finished
  bool        finished() const;
  const Plan& result() const;

 private:
  struct Job;
  std::unique_ptr<Job> job_;
  Plan                 result_;
};

// The same, in one go: for callers that asked a question and are waiting
// for the answer. Bounded, so a route that cannot be finished in reasonable
// time comes back with what it has, its later legs marked unverified.
Plan PlanPath(const Vec3& from, const Vec3& to);

// When nothing can be planned, the best way to set off anyway: sixteen
// short walks fanned out from here, scored by how far each gets and how much
// of that is toward the target. The way a person feels along when the map is
// no help. Game thread only; false when no direction is walkable at all.
bool BestDirection(const Vec3& here, const Vec3& target, Vec3* out,
                   std::string* why);

// Something the walker met that the plan did not know about: a parked car,
// a closed gate, a wall the collision test slips through. Remembered for a
// while and routed around by every plan after, so the next route is a
// different route and not the same one again.
void RememberObstacle(const Vec3& at, const char* what);
void ForgetObstacles();
// The ones still remembered, for the field to paint.
std::vector<Vec3> RememberedObstacles();

// The last plan asked for, from anywhere - the panel draws it in the world.
// Also the target, the reach fan the panel maintains itself, and what the
// walker's whiskers last saw.
struct DebugState {
  bool             has_target = false;
  Vec3             target;
  Plan             plan;
  // Which directions from the player are walkable for a few metres, and how
  // far. Sixteen spokes.
  std::vector<Vec3> fan_ends;
  std::vector<bool> fan_ok;
  std::vector<game::PathNode> nodes;  // ped nodes near the player, if wanted
  // The walker's whiskers: where each ended, whether it was clear, and
  // where they were cast from.
  Vec3              whisker_origin;
  std::vector<Vec3> whisker_ends;
  std::vector<bool> whisker_clear;
  std::vector<Vec3> obstacles;        // remembered, still in force
};
void SetDebugPlan(const Vec3& target, const Plan& plan);
void SetDebugFan(std::vector<Vec3> ends, std::vector<bool> ok);
void SetDebugNodes(std::vector<game::PathNode> nodes);
void SetDebugWhiskers(const Vec3& origin, std::vector<Vec3> ends,
                      std::vector<bool> clear);
void ClearDebug();
DebugState GetDebug();

}  // namespace gtabot::nav
