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
//   PlanPath(a, b) - a route. Straight if the straight line is walkable;
//                    otherwise along the game's own ped nodes - the pavements
//                    its pedestrians use - joined to a and b by walkable legs
//                    and then pulled tight so he does not visit every node.
//
// Every verdict says why. "No ground" fifty metres away usually means the
// game has not streamed that far, not that there is a hole; the caller is
// told the difference by the distance.
//
// Positions are where a ped's origin sits, about a metre above his feet,
// which is how the game reports the player's own position. Points that come
// in at ground level are lifted; points that go out are snapped to the
// ground the game found.
//
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
};

Verdict Standable(const Vec3& p);
Verdict Walkable(const Vec3& a, const Vec3& b);

struct Leg {
  Vec3        from, to;
  bool        ok        = false;
  bool        verified  = false;  // false when the call budget ran out first
  bool        via_graph = false;
  std::string why;
};

struct Plan {
  bool             ok = false;
  std::vector<Vec3> waypoints;   // from, ..., to
  std::vector<Leg>  legs;
  float            length_m    = 0;
  int              graph_nodes = 0;   // ped nodes the route passed through
  int              game_calls  = 0;
  std::string      note;
};

// Game thread only, and not cheap: a long route is thousands of calls into
// the game. Bounded by a call budget; what could not be verified in time is
// marked rather than assumed.
Plan PlanPath(const Vec3& from, const Vec3& to);

// The last plan asked for, from anywhere - the panel draws it in the world.
// Also the target and the reach fan the panel maintains itself.
struct DebugState {
  bool             has_target = false;
  Vec3             target;
  Plan             plan;
  // Which directions from the player are walkable for a few metres, and how
  // far. Sixteen spokes.
  std::vector<Vec3> fan_ends;
  std::vector<bool> fan_ok;
  std::vector<game::PathNode> nodes;  // ped nodes near the player, if wanted
};
void SetDebugPlan(const Vec3& target, const Plan& plan);
void SetDebugFan(std::vector<Vec3> ends, std::vector<bool> ok);
void SetDebugNodes(std::vector<game::PathNode> nodes);
void ClearDebug();
DebugState GetDebug();

}  // namespace gtabot::nav
