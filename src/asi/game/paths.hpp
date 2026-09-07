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
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::game {

constexpr int kPathAreas = 64;

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

}  // namespace gtabot::game
