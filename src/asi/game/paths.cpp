#include "game/paths.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "game/exe.hpp"
#include "log.hpp"
#include "state/memory.hpp"
#include "types.hpp"

namespace gtabot::game {
namespace {

// CPathFind, per the public headers for 1.0 US. The starting guess for where
// to look; nothing is read from it until the arrays inside have been
// recognised by what they contain.
// CPathFind, from the reversed declaration rather than from a search.
//
//   CNodeAddress             info;                      0x000
//   CPathNode*               m_apNodesSearchLists[512]; 0x004
//   CPathNode*               m_pPathNodes[72];          0x804
//   CCarPathLink*            m_pNaviNodes[72];          0x924
//   CNodeAddress*            m_pNodeLinks[72];          0xA44
//   unsigned char*           m_pLinkLengths[72];        0xB64
//   CPathIntersectionInfo*   m_pPathIntersections[72];  0xC84
//   CCarPathLinkAddress*     m_pNaviLinks[64];          0xDA4
//   void*                    field_EA4[64];             0xEA4  <- names its own
//   unsigned int             m_dwNumNodes[72];          0xFA4
//   unsigned int             m_dwNumVehicleNodes[72];   0x10C4
//   unsigned int             m_dwNumPedNodes[72];       0x11E4
//
// Two things this settles that a search never could. There are seventy-two
// areas, not sixty-four - sixty-four of map and eight of interiors - so the
// stride between these arrays is 288 bytes, not 256. And the counts are
// 32-bit, not 16. Looking for three 64-entry arrays 256 bytes apart was
// looking for something that is not there, which is why it never found them.
constexpr std::uint32_t kThePaths        = 0x96F050;
constexpr std::uint32_t kPathNodesArray  = kThePaths + 0x804;
constexpr std::uint32_t kNodeLinksFromNodes        = 0x240;
constexpr std::uint32_t kNumNodesFromNodes         = 0x7A0;
constexpr std::uint32_t kNumVehicleNodesFromNodes  = 0x8C0;
constexpr std::uint32_t kNumPedNodesFromNodes      = 0x9E0;

// One node is 28 bytes: two words nobody reads, the position as three
// signed shorts in eighths of a metre, a search field, the index of the
// first link, the area, the node's own index, width, a fill byte, and flags
// whose low four bits are the link count.
constexpr std::uint32_t kNodeSize  = 28;
constexpr std::uint32_t kNodeX     = 0x08;
constexpr std::uint32_t kNodeY     = 0x0A;
constexpr std::uint32_t kNodeZ     = 0x0C;
constexpr std::uint32_t kNodeBase  = 0x10;
constexpr std::uint32_t kNodeArea  = 0x12;
constexpr std::uint32_t kNodeIndex = 0x14;
constexpr std::uint32_t kNodeFlags = 0x18;
constexpr float         kNodeScale = 1.0f / 8.0f;
// A link is an area and an index, a word each.
constexpr std::uint32_t kLinkSize = 4;

// The map is 6 km square, split eight by eight.
constexpr float kWorldHalf  = 3000.0f;
constexpr float kAreaSide   = 750.0f;
constexpr float kMaxNodeZ   = 1500.0f;
constexpr float kMinNodeZ   = -500.0f;
// A link that leads further than this is not a link, it is a misread.
constexpr float kMaxLinkMetres = 400.0f;
constexpr std::uint32_t kMaxNodesPerArea = 20000;

// The check the positions have to pass: from a street, the nearest ped node
// is metres away, not hundreds.
constexpr float kSelfCheckRadius = 150.0f;

constexpr unsigned long long kRetryAfterMs   = 5000;
constexpr unsigned long long kResolveBudgetMs = 40;

PathLayout         g_layout;
bool               g_resolved = false;
unsigned long long g_last_attempt_ms = 0;
// The null pattern of the node array at resolve time. When it changes the
// game has loaded or dropped an area, and the cached counts are stale.
std::uint32_t      g_loaded_mask = 0;

int AreaOf(float x, float y) {
  int ax = static_cast<int>((x + kWorldHalf) / kAreaSide);
  int ay = static_cast<int>((y + kWorldHalf) / kAreaSide);
  ax = ax < 0 ? 0 : ax > 7 ? 7 : ax;
  ay = ay < 0 ? 0 : ay > 7 ? 7 : ay;
  return ay * 8 + ax;
}

bool PlausiblePosition(const Vec3& p) {
  return p.x > -kWorldHalf - 500 && p.x < kWorldHalf + 500 &&
         p.y > -kWorldHalf - 500 && p.y < kWorldHalf + 500 &&
         p.z > kMinNodeZ && p.z < kMaxNodeZ;
}

bool ReadNodeAt(std::uintptr_t at, PathNode* out) {
  unsigned char raw[kNodeSize];
  if (asi::mem::ReadGuarded(at, raw, kNodeSize) != kNodeSize) return false;
  std::int16_t  x, y, z;
  std::uint16_t base, area, index;
  std::uint32_t flags;
  std::memcpy(&x, raw + kNodeX, 2);
  std::memcpy(&y, raw + kNodeY, 2);
  std::memcpy(&z, raw + kNodeZ, 2);
  std::memcpy(&base, raw + kNodeBase, 2);
  std::memcpy(&area, raw + kNodeArea, 2);
  std::memcpy(&index, raw + kNodeIndex, 2);
  std::memcpy(&flags, raw + kNodeFlags, 4);
  out->pos        = Vec3{x * kNodeScale, y * kNodeScale, z * kNodeScale};
  out->area       = area;
  out->index      = index;
  out->base_link  = base;
  out->link_count = static_cast<std::uint8_t>(flags & 0xF);
  out->flags      = flags;
  return true;
}

// One count, read at the width the layout uses.
std::uint32_t CountAt(std::uintptr_t base, int area, int width) {
  if (width == 2) {
    std::uint16_t value = 0;
    return asi::mem::Read<std::uint16_t>(base + area * 2, &value) ? value : 0;
  }
  std::uint32_t value = 0;
  return asi::mem::Read<std::uint32_t>(base + area * 4, &value) ? value : 0;
}

// A whole 64-entry count array, widened to 32 bits.
bool LoadCounts(std::uintptr_t base, int width, std::uint32_t* out) {
  if (width == 2) {
    std::uint16_t narrow[kPathAreas];
    if (asi::mem::ReadGuarded(base, narrow, sizeof(narrow)) != sizeof(narrow))
      return false;
    for (int i = 0; i < kPathAreas; ++i) out[i] = narrow[i];
    return true;
  }
  return asi::mem::ReadGuarded(base, out, kPathAreas * 4) == kPathAreas * 4;
}

bool ReadPointers(std::uintptr_t at, std::uint32_t* out) {
  return asi::mem::ReadGuarded(at, out, kPathAreas * 4) == kPathAreas * 4;
}

// How many nodes an area's array actually holds, read off the array itself:
// every node carries its own index, so the array ends where that stops
// matching. This needs no count table at all.
std::uint32_t WalkedCount(std::uintptr_t pointer, int area) {
  std::uint32_t n = 0;
  PathNode node;
  while (n < kMaxNodesPerArea &&
         ReadNodeAt(pointer + n * kNodeSize, &node) && node.area == area &&
         node.index == n)
    ++n;
  return n;
}

// Written once when the counts cannot be found: every 64-word row around
// the node array, with the entries for the loaded areas shown - the count
// tables are the rows whose entries for exactly those areas are the numbers
// the arrays were just measured to hold.
void DumpAround(std::uintptr_t nodes, const std::uint32_t* node_pointers) {
  static bool written = false;
  if (written) return;
  written = true;

  const std::string path = ModuleDirectory() + "bot.paths-dump.txt";
  std::ofstream file(path, std::ios::trunc);
  if (!file) return;

  const std::uintptr_t base = Detect().base;
  file << "gtabot path graph dump" << std::endl
       << "======================" << std::endl << std::endl
       << "node array at gta_sa.exe+0x" << std::hex << (nodes - base) << std::dec
       << std::endl << std::endl << "loaded areas, with the node count walked "
       << "off each array:" << std::endl;
  int loaded[kPathAreas];
  int loaded_count = 0;
  for (int i = 0; i < kPathAreas; ++i) {
    if (node_pointers[i] == 0) continue;
    loaded[loaded_count++] = i;
    file << "  area " << i << "  nodes at 0x" << std::hex << node_pointers[i]
         << std::dec << "  holds " << WalkedCount(node_pointers[i], i)
         << " nodes" << std::endl;
  }

  file << std::endl << "64-word rows around the node array (offset from it, "
       << "how many entries are non-zero, then the entries for the loaded "
       << "areas):" << std::endl;
  for (std::ptrdiff_t offset = -0x1000; offset <= 0x1800; offset += 0x100) {
    std::uint32_t row[kPathAreas];
    if (asi::mem::ReadGuarded(nodes + offset, row, sizeof(row)) != sizeof(row))
      continue;
    int nonzero = 0;
    for (int i = 0; i < kPathAreas; ++i)
      if (row[i] != 0) ++nonzero;
    char head[64];
    std::snprintf(head, sizeof(head), "  %+6d  nonzero %2d  ",
                  static_cast<int>(offset), nonzero);
    file << head;
    for (int k = 0; k < loaded_count; ++k) {
      char cell[48];
      std::snprintf(cell, sizeof(cell), "[%d]=0x%08X ", loaded[k],
                    row[loaded[k]]);
      file << cell;
    }
    file << std::endl;
  }
  file << std::endl
       << "the count region as int16, per loaded area (offset from the node "
       << "array; the counts are the columns that read 273 for area 5 and "
       << "2410 for area 13, etc.):" << std::endl;
  for (std::ptrdiff_t offset = 0x600; offset <= 0xA00; offset += 2) {
    char head[32];
    std::snprintf(head, sizeof(head), "  +0x%03X ", static_cast<unsigned>(offset));
    file << head;
    for (int k = 0; k < loaded_count; ++k) {
      std::uint16_t v = 0;
      asi::mem::Read<std::uint16_t>(nodes + offset + loaded[k] * 2, &v);
      char cell[24];
      std::snprintf(cell, sizeof(cell), "[%d]=%-5u ", loaded[k], v);
      file << cell;
    }
    file << std::endl;
  }
  file << std::endl;
  LOG_INFO("wrote {}", path);
}

// Said once per distinct reason, so a graph that never resolves leaves a
// trail rather than a gap.
void NoteFailure(const std::string& note) {
  static std::string last;
  if (note == last) return;
  last = note;
  LOG_WARN("path graph: {}", note);
}

// Three arrays of 64 counts where one is the sum of the other two, and the
// total for every loaded area names an array of exactly that many nodes -
// the last node of the array carries its own index, which has to be the
// count less one.
bool CountsAgree(std::uintptr_t all, std::uintptr_t vehicle, std::uintptr_t ped,
                 int width, const std::uint32_t* node_pointers) {
  std::uint32_t a[kPathAreas], v[kPathAreas], p[kPathAreas];
  if (!LoadCounts(all, width, a)) return false;
  if (!LoadCounts(vehicle, width, v)) return false;
  if (!LoadCounts(ped, width, p)) return false;
  int checked = 0;
  for (int i = 0; i < kPathAreas; ++i) {
    if (a[i] != v[i] + p[i]) return false;
    if (a[i] > kMaxNodesPerArea) return false;
    if (node_pointers[i] == 0) continue;
    if (a[i] == 0) return false;
    PathNode last;
    if (!ReadNodeAt(node_pointers[i] + (a[i] - 1) * kNodeSize, &last))
      return false;
    if (last.index != a[i] - 1 || last.area != i) return false;
    ++checked;
  }
  return checked > 0;
}

// The link table: 64 pointers with the same null pattern as the node array,
// whose entries, for a node with links, lead to nodes a walk away.
bool IsLinkArray(std::uintptr_t at, const std::uint32_t* node_pointers,
                 std::uintptr_t count_all) {
  std::uint32_t entries[kPathAreas];
  if (!ReadPointers(at, entries)) return false;
  std::uint32_t totals[kPathAreas];
  if (asi::mem::ReadGuarded(count_all, totals, sizeof(totals)) != sizeof(totals))
    return false;

  int verified = 0;
  for (int i = 0; i < kPathAreas; ++i) {
    if ((entries[i] == 0) != (node_pointers[i] == 0)) return false;
    if (entries[i] == 0) continue;
    if (entries[i] < 0x10000u || entries[i] >= 0xC0000000u) return false;

    // A few nodes of this area that have links; each link must land on a
    // node that exists and is nearby.
    int tried = 0;
    for (std::uint32_t n = 0; n < totals[i] && tried < 3; n += 37) {
      PathNode node;
      if (!ReadNodeAt(node_pointers[i] + n * kNodeSize, &node)) return false;
      if (node.link_count == 0) continue;
      ++tried;
      for (int k = 0; k < node.link_count; ++k) {
        std::uint16_t link_area = 0, link_index = 0;
        const std::uintptr_t link =
            entries[i] + (node.base_link + k) * kLinkSize;
        if (!asi::mem::Read<std::uint16_t>(link, &link_area)) return false;
        if (!asi::mem::Read<std::uint16_t>(link + 2, &link_index)) return false;
        if (link_area >= kPathAreas) return false;
        if (node_pointers[link_area] == 0) continue;  // not loaded; fine
        if (link_index >= totals[link_area]) return false;
        PathNode other;
        if (!ReadNodeAt(node_pointers[link_area] + link_index * kNodeSize,
                        &other))
          return false;
        if (other.index != link_index || other.area != link_area) return false;
        const float dx = other.pos.x - node.pos.x;
        const float dy = other.pos.y - node.pos.y;
        if (std::sqrt(dx * dx + dy * dy) > kMaxLinkMetres) return false;
      }
    }
    verified += tried;
  }
  return verified > 0;
}

std::uint32_t LoadedMask(const std::uint32_t* node_pointers) {
  std::uint32_t mask = 0;
  for (int i = 0; i < 32; ++i)
    if (node_pointers[i] != 0) mask |= 1u << i;
  for (int i = 32; i < kPathAreas; ++i)
    if (node_pointers[i] != 0) mask ^= 0x9E3779B9u * static_cast<std::uint32_t>(i);
  return mask;
}

}  // namespace

const PathLayout& CachedPaths() { return g_layout; }

void ForgetPaths() {
  g_resolved = false;
  g_layout   = PathLayout{};
}

const PathLayout& ResolvePaths(const Vec3& player) {
  // Areas come and go as the player moves; when the set changes, the counts
  // were read for a different world and get read again.
  if (g_resolved) {
    std::uint32_t pointers[kPathAreas];
    if (ReadPointers(g_layout.nodes, pointers) &&
        LoadedMask(pointers) == g_loaded_mask)
      return g_layout;
    g_resolved = false;
  }

  const unsigned long long now = GetTickCount64();
  if (g_last_attempt_ms != 0 && now - g_last_attempt_ms < kRetryAfterMs)
    return g_layout;
  g_last_attempt_ms = now;

  PathLayout layout;
  const std::uintptr_t nodes = At(kPathNodesArray);
  if (nodes == 0) {
    layout.note = "the executable is not the build these addresses are for";
    g_layout = layout;
    return g_layout;
  }

  // Nothing is searched for any more. CPathFind's layout is declared, and
  // every offset below follows from it by arithmetic that the data then has
  // to agree with. Sweeping the executable for these arrays was both slower
  // and, on a server that watches for exactly that, a poor idea.
  layout.nodes         = nodes;
  layout.links         = nodes + kNodeLinksFromNodes;
  layout.count_all     = nodes + kNumNodesFromNodes;
  layout.count_vehicle = nodes + kNumVehicleNodesFromNodes;
  layout.count_ped     = nodes + kNumPedNodesFromNodes;
  layout.count_width   = 4;

  std::uint32_t node_pointers[kPathAreas];
  if (!ReadPointers(nodes, node_pointers)) {
    layout.note = "the node array is not readable - the graph is not loaded";
    g_layout = layout;
    return g_layout;
  }
  for (int i = 0; i < kPathAreas; ++i)
    if (node_pointers[i] != 0) ++layout.loaded_areas;
  if (layout.loaded_areas == 0) {
    layout.note = "no path area is loaded yet";
    g_layout = layout;
    return g_layout;
  }

  // The layout is declared, not believed. Every loaded area has to agree that
  // its total is its vehicle nodes plus its ped nodes, and that the last node
  // of its array carries the index that total implies.
  if (!CountsAgree(layout.count_all, layout.count_vehicle, layout.count_ped,
                   layout.count_width, node_pointers)) {
    layout.note = "the declared count tables do not describe the nodes that "
                  "are loaded - this is not the CPathFind these offsets are for";
    NoteFailure(layout.note);
    DumpAround(nodes, node_pointers);
    g_layout = layout;
    g_resolved = true;   // the offsets do not change by waiting
    return g_layout;
  }
  if (!IsLinkArray(layout.links, node_pointers, layout.count_all)) {
    layout.note = "the counts check out but the link table does not lead to "
                  "nearby nodes";
    NoteFailure(layout.note);
    g_layout = layout;
    g_resolved = true;
    return g_layout;
  }

  std::uint32_t ped[kPathAreas];
  LoadCounts(layout.count_ped, layout.count_width, ped);
  for (int i = 0; i < kPathAreas; ++i)
    if (node_pointers[i] != 0) layout.ped_nodes_loaded += static_cast<int>(ped[i]);

  layout.valid  = true;
  g_layout      = layout;
  g_resolved    = true;
  g_loaded_mask = LoadedMask(node_pointers);

  // The check that the positions decode right: the nearest ped node to a
  // player on a street is metres away.
  const std::vector<PathNode> nearest = PedNodesNear(player, kSelfCheckRadius, 1);
  if (!nearest.empty()) {
    const float dx = nearest[0].pos.x - player.x;
    const float dy = nearest[0].pos.y - player.y;
    g_layout.nearest_ped_node_m = std::sqrt(dx * dx + dy * dy);
  }
  const int player_area = AreaOf(player.x, player.y);
  g_layout.note = node_pointers[player_area] != 0
                      ? "resolved"
                      : "resolved, but the player's own area is not loaded";

  LOG_INFO("path graph resolved: nodes=0x{:08X} links=0x{:08X} counts=0x{:08X} "
           "{} areas loaded, {} ped nodes, nearest {:.1f} m, {}",
           g_layout.nodes, g_layout.links, g_layout.count_all,
           g_layout.loaded_areas, g_layout.ped_nodes_loaded,
           g_layout.nearest_ped_node_m, g_layout.note);
  return g_layout;
}

bool ReadNode(std::uint16_t area, std::uint16_t index, PathNode* out) {
  if (!g_layout.valid || area >= kPathAreas) return false;
  std::uint32_t pointer = 0;
  if (!asi::mem::Read<std::uint32_t>(g_layout.nodes + area * 4, &pointer) ||
      pointer == 0)
    return false;
  const std::uint32_t total =
      CountAt(g_layout.count_all, area, g_layout.count_width);
  if (index >= total) return false;
  const std::uint32_t vehicle =
      CountAt(g_layout.count_vehicle, area, g_layout.count_width);
  if (!ReadNodeAt(pointer + index * kNodeSize, out)) return false;
  out->ped = index >= vehicle;
  return out->area == area && out->index == index;
}

int ReadLinks(const PathNode& node, PathLink* out, int max) {
  if (!g_layout.valid || node.area >= kPathAreas) return 0;
  std::uint32_t table = 0;
  if (!asi::mem::Read<std::uint32_t>(g_layout.links + node.area * 4, &table) ||
      table == 0)
    return 0;
  int written = 0;
  for (int k = 0; k < node.link_count && written < max; ++k) {
    const std::uintptr_t link = table + (node.base_link + k) * kLinkSize;
    PathLink entry;
    if (!asi::mem::Read<std::uint16_t>(link, &entry.area)) break;
    if (!asi::mem::Read<std::uint16_t>(link + 2, &entry.index)) break;
    if (entry.area >= kPathAreas) continue;
    std::uint32_t pointer = 0;
    if (!asi::mem::Read<std::uint32_t>(g_layout.nodes + entry.area * 4,
                                       &pointer) ||
        pointer == 0)
      continue;  // leads into an area that is not loaded
    out[written++] = entry;
  }
  return written;
}

std::vector<PathNode> PedNodesNear(const Vec3& at, float radius,
                                   std::size_t max) {
  std::vector<PathNode> found;
  if (!g_layout.valid) return found;

  std::uint32_t pointers[kPathAreas], total[kPathAreas], vehicle[kPathAreas];
  if (!ReadPointers(g_layout.nodes, pointers)) return found;
  if (!LoadCounts(g_layout.count_all, g_layout.count_width, total)) return found;
  if (!LoadCounts(g_layout.count_vehicle, g_layout.count_width, vehicle))
    return found;

  struct Hit {
    float    distance;
    PathNode node;
  };
  std::vector<Hit> hits;
  std::vector<unsigned char> block;
  const float radius_squared = radius * radius;

  for (int i = 0; i < kPathAreas; ++i) {
    if (pointers[i] == 0 || total[i] == 0 || vehicle[i] >= total[i]) continue;
    // The ped section of the area, copied out in one go rather than read a
    // node at a time: a few thousand nodes are a few kilobytes.
    const std::uint32_t first = vehicle[i];
    const std::uint32_t count = total[i] - vehicle[i];
    if (count > kMaxNodesPerArea) continue;
    block.resize(count * kNodeSize);
    const std::size_t got = asi::mem::ReadGuarded(
        pointers[i] + first * kNodeSize, block.data(), block.size());
    const std::uint32_t usable = static_cast<std::uint32_t>(got / kNodeSize);

    for (std::uint32_t n = 0; n < usable; ++n) {
      const unsigned char* raw = block.data() + n * kNodeSize;
      std::int16_t x, y, z;
      std::memcpy(&x, raw + kNodeX, 2);
      std::memcpy(&y, raw + kNodeY, 2);
      std::memcpy(&z, raw + kNodeZ, 2);
      const float dx = x * kNodeScale - at.x;
      const float dy = y * kNodeScale - at.y;
      const float dz = z * kNodeScale - at.z;
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > radius_squared) continue;

      Hit hit;
      hit.distance = std::sqrt(d2);
      hit.node.pos = Vec3{x * kNodeScale, y * kNodeScale, z * kNodeScale};
      std::memcpy(&hit.node.base_link, raw + kNodeBase, 2);
      std::memcpy(&hit.node.area, raw + kNodeArea, 2);
      std::memcpy(&hit.node.index, raw + kNodeIndex, 2);
      std::memcpy(&hit.node.flags, raw + kNodeFlags, 4);
      hit.node.link_count = static_cast<std::uint8_t>(hit.node.flags & 0xF);
      hit.node.ped = true;
      hits.push_back(hit);
    }
  }

  std::sort(hits.begin(), hits.end(),
            [](const Hit& a, const Hit& b) { return a.distance < b.distance; });
  if (hits.size() > max) hits.resize(max);
  found.reserve(hits.size());
  for (const Hit& hit : hits) found.push_back(hit.node);
  return found;
}

}  // namespace gtabot::game
