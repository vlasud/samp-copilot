#pragma once
//
// Reads the game's own navigation graph.
//
// GTA keeps the world's roads and pavements as a graph of nodes - the paths
// its pedestrians walk and its traffic drives - split into an 8x8 grid of
// areas and loaded a few areas at a time around the player. The ped nodes
// are what an NPC follows when it walks somewhere, which makes them the
// definition of a natural route: a character on them walks where the people
// around him walk.
//
// Where the graph lives in memory is taken from the public headers as a
// starting guess and then established from the data, the way the player pool
// was. Every node records which area's array it belongs to, so the array of
// per-area pointers is the one whose i-th entry leads to nodes that say "i".
// The counts are the three arrays where one is the sum of the other two, and
// the link table is the one whose entries lead somewhere close by.
//
// Only loaded areas can be read. That is not a limitation to work around: it
// is the game telling us how far it can see, and a route beyond it is not
// one the character can walk yet either.
//
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::game {

// Sixty-four map areas and eight interior ones. The arrays inside CPathFind
// are all sized for both, which is what makes the stride between them 288
// bytes rather than 256.
constexpr int kPathAreas = 72;
constexpr int kPathMapAreas = 64;

struct PathNode {
  Vec3          pos;
  std::uint16_t area  = 0;
  std::uint16_t index = 0;
  std::uint16_t base_link  = 0;
  std::uint8_t  link_count = 0;
  std::uint32_t flags = 0;
  bool          ped   = false;   // the rest are vehicle nodes
};

struct PathLink {
  std::uint16_t area  = 0;
  std::uint16_t index = 0;
};

struct PathLayout {
  bool           valid = false;
  std::uintptr_t nodes = 0;   // uintptr_t[64], one array of nodes per area
  std::uintptr_t links = 0;   // uintptr_t[64], one array of links per area
  std::uintptr_t count_all = 0, count_vehicle = 0, count_ped = 0;
  // 2 or 4: SA stores these counts as int16, but a build could widen them, so
  // the width is established from the data rather than assumed.
  int            count_width = 4;
  int            loaded_areas = 0;
  int            ped_nodes_loaded = 0;
  // The nearest ped node to the player at resolve time, as the check that
  // the positions decode correctly: on a street it is metres away.
  float          nearest_ped_node_m = -1;
  std::string    note;
};

// Resolved once and cached, re-resolved when the loaded areas change. Needs
// the player's position for its self-check. Game thread only.
const PathLayout& ResolvePaths(const Vec3& player);
const PathLayout& CachedPaths();
void ForgetPaths();

bool ReadNode(std::uint16_t area, std::uint16_t index, PathNode* out);
// The nodes this one connects to. Only those in loaded areas are returned,
// since the rest cannot be read anyway.
int ReadLinks(const PathNode& node, PathLink* out, int max);
// Ped nodes within `radius` of a point, nearest first. Game thread only.
std::vector<PathNode> PedNodesNear(const Vec3& at, float radius,
                                   std::size_t max);

// The loaded graph copied out in one go, so a search over it costs memory
// reads rather than a validated read per node.
//
// A route search touches thousands of nodes and every link of each; done a
// node at a time against the game's memory that is tens of thousands of
// VirtualQuery calls, and a plan across a district spent fifty milliseconds
// in it before a single call into the game was made. Copying the areas
// first is a few hundred kilobytes and a handful of reads.
struct Graph {
  struct Area {
    bool                  loaded = false;
    std::uint32_t         total = 0, vehicle = 0;   // nodes; ped ones follow
    std::vector<PathNode> nodes;                    // all of them, by index
    std::vector<PathLink> links;                    // the area's link table
  };
  bool  valid = false;
  Area  areas[kPathAreas];
  // Every node by the 24-metre square it stands in, so "what is near this
  // point" is a look at nine squares rather than at every node loaded.
  std::unordered_map<std::uint32_t, std::vector<PathLink>> squares;

  const PathNode* Node(std::uint16_t area, std::uint16_t index) const;
  // The ped nodes this one links to that are in loaded areas.
  int Links(const PathNode& node, PathLink* out, int max) const;
  // Ped nodes within `radius` of a point, nearest first.
  std::vector<PathNode> PedNodesNear(const Vec3& at, float radius,
                                     std::size_t max) const;
  // Ped nodes within `radius` of a point, nearest first, by reference and
  // fast: the squares, not a sweep. Radius up to 24 m.
  std::vector<PathLink> PedNodesAround(const Vec3& at, float radius,
                                       std::size_t max) const;
};
// Game thread only. Empty (valid=false) until the layout has resolved.
Graph SnapshotGraph();

}  // namespace gtabot::game
