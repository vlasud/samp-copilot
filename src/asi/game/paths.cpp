#include "game/paths.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "game/exe.hpp"
#include "log.hpp"
#include "state/memory.hpp"

namespace gtabot::game {
namespace {

// CPathFind, per the public headers for 1.0 US. The starting guess for where
// to look; nothing is read from it until the arrays inside have been
// recognised by what they contain.
constexpr std::uint32_t kThePaths     = 0x96F050;
constexpr std::uint32_t kThePathsSize = 0x3C80;

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

// Whether `pointer` leads to an array of nodes that say they are area `area`.
bool LeadsToNodesOf(std::uintptr_t pointer, int area) {
  if (pointer == 0) return false;
  PathNode first;
  if (!ReadNodeAt(pointer, &first)) return false;
  return first.area == area && first.index == 0 &&
         PlausiblePosition(first.pos);
}

bool ReadPointers(std::uintptr_t at, std::uint32_t* out) {
  return asi::mem::ReadGuarded(at, out, kPathAreas * 4) == kPathAreas * 4;
}

// The per-area node array: 64 pointers, null for areas not loaded, and every
// other one leading to nodes that name that area.
bool IsNodeArray(std::uintptr_t at, int* loaded) {
  std::uint32_t entries[kPathAreas];
  if (!ReadPointers(at, entries)) return false;
  int present = 0;
  for (int i = 0; i < kPathAreas; ++i) {
    if (entries[i] == 0) continue;
    if (!LeadsToNodesOf(entries[i], i)) return false;
    ++present;
  }
  if (present == 0) return false;
  *loaded = present;
  return true;
}

std::uintptr_t FindNodeArray(std::uintptr_t begin, std::uintptr_t end,
                             unsigned long long deadline, int* loaded) {
  // Any pointer to a block whose first node says "area k, index 0" puts the
  // array k slots before it. Cheaper than testing every offset as a start.
  //
  // Read a chunk at a time rather than a word at a time: a word at a time
  // asks the kernel about every one of them, and over the executable's data
  // that took longer than the budget allowed, every time, from the start -
  // a fallback that could never finish.
  constexpr std::size_t kChunkWords = 2048;
  std::uint32_t chunk[kChunkWords];
  for (std::uintptr_t base = begin; base + 4 <= end; base += kChunkWords * 4) {
    if (GetTickCount64() > deadline) return 0;
    const std::size_t want =
        (end - base < kChunkWords * 4 ? end - base : kChunkWords * 4) & ~3u;
    const std::size_t got = asi::mem::ReadGuarded(base, chunk, want);
    for (std::size_t i = 0; i < got / 4; ++i) {
      const std::uint32_t value = chunk[i];
      if (value < 0x10000u || value >= 0xC0000000u || value % 4 != 0) continue;
      PathNode first;
      if (!ReadNodeAt(value, &first)) continue;
      if (first.index != 0 || first.area >= kPathAreas) continue;
      if (!PlausiblePosition(first.pos)) continue;
      const std::uintptr_t at = base + i * 4;
      const std::uintptr_t start = at - first.area * 4;
      if (start < begin) continue;
      if (IsNodeArray(start, loaded)) return start;
    }
    if (got < want) break;
  }
  return 0;
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
                 const std::uint32_t* node_pointers) {
  std::uint32_t a[kPathAreas], v[kPathAreas], p[kPathAreas];
  if (asi::mem::ReadGuarded(all, a, sizeof(a)) != sizeof(a)) return false;
  if (asi::mem::ReadGuarded(vehicle, v, sizeof(v)) != sizeof(v)) return false;
  if (asi::mem::ReadGuarded(ped, p, sizeof(p)) != sizeof(p)) return false;
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

bool FindCounts(std::uintptr_t begin, std::uintptr_t end,
                const std::uint32_t* node_pointers, PathLayout* layout) {
  for (std::uintptr_t at = begin; at + kPathAreas * 12 <= end; at += 4) {
    const std::uintptr_t first = at, second = at + kPathAreas * 4,
                         third = at + kPathAreas * 8;
    // The total may be declared first or last; the data decides which.
    if (CountsAgree(first, second, third, node_pointers)) {
      layout->count_all = first;
      layout->count_vehicle = second;
      layout->count_ped = third;
      return true;
    }
    if (CountsAgree(third, first, second, node_pointers)) {
      layout->count_all = third;
      layout->count_vehicle = first;
      layout->count_ped = second;
      return true;
    }
  }
  return false;
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

std::uintptr_t FindLinkArray(std::uintptr_t begin, std::uintptr_t end,
                             const std::uint32_t* node_pointers,
                             std::uintptr_t count_all, std::uintptr_t not_this) {
  for (std::uintptr_t at = begin; at + kPathAreas * 4 <= end; at += 4) {
    if (at == not_this) continue;
    if (IsLinkArray(at, node_pointers, count_all)) return at;
  }
  return 0;
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
  // were read for a different world and get established again.
  if (g_resolved) {
    std::uint32_t pointers[kPathAreas];
    if (ReadPointers(g_layout.nodes, pointers) &&
        LoadedMask(pointers) == g_loaded_mask)
      return g_layout;
    LOG_INFO("path areas changed - re-reading the graph");
    g_resolved = false;
  }

  const unsigned long long now = GetTickCount64();
  if (g_last_attempt_ms != 0 && now - g_last_attempt_ms < kRetryAfterMs)
    return g_layout;
  g_last_attempt_ms = now;
  const unsigned long long deadline = now + kResolveBudgetMs;

  PathLayout layout;
  const std::uintptr_t the_paths = At(kThePaths);
  if (the_paths == 0) {
    layout.note = "the executable is not the build the graph's address is for";
    g_layout = layout;
    return g_layout;
  }

  // Search the structure the headers name first, then the whole of the
  // executable's data if that comes up empty.
  const std::uintptr_t window_begin = the_paths;
  const std::uintptr_t window_end   = the_paths + kThePathsSize;
  int loaded = 0;
  std::uintptr_t nodes = FindNodeArray(window_begin, window_end, deadline, &loaded);
  if (nodes == 0) {
    const asi::mem::Module exe = asi::mem::FindModule(nullptr);
    for (const asi::mem::Region& region : asi::mem::ReadableRegions(&exe)) {
      if (!region.is_writable) continue;
      nodes = FindNodeArray(region.base, region.base + region.size, deadline,
                            &loaded);
      if (nodes != 0) break;
    }
  }
  if (nodes == 0) {
    layout.note = "no per-area node array found - the graph is not loaded "
                  "yet, or this is not the layout the headers describe";
    NoteFailure(layout.note);
    g_layout = layout;
    return g_layout;
  }
  layout.nodes        = nodes;
  layout.loaded_areas = loaded;

  std::uint32_t node_pointers[kPathAreas];
  ReadPointers(nodes, node_pointers);

  // The other arrays live in the same structure, so they are looked for
  // around the one just found rather than anywhere.
  const std::uintptr_t near_begin = nodes > 0x4000 ? nodes - 0x4000 : 0;
  const std::uintptr_t near_end   = nodes + 0x4000;
  if (!FindCounts(near_begin, near_end, node_pointers, &layout)) {
    char where[96];
    std::snprintf(where, sizeof(where), " (node array at gta_sa.exe+0x%X, %d areas loaded)",
                  static_cast<unsigned>(nodes - Detect().base), loaded);
    layout.note = "node array found, but no three count arrays where one is "
                  "the sum of the other two" + std::string(where);
    NoteFailure(layout.note);
    g_layout = layout;
    return g_layout;
  }
  layout.links = FindLinkArray(near_begin, near_end, node_pointers,
                               layout.count_all, nodes);
  if (layout.links == 0) {
    char where[128];
    std::snprintf(where, sizeof(where),
                  " (nodes at gta_sa.exe+0x%X, counts at +0x%X)",
                  static_cast<unsigned>(nodes - Detect().base),
                  static_cast<unsigned>(layout.count_all - Detect().base));
    layout.note = "node array and counts found, but no link table whose "
                  "entries lead to nearby nodes" + std::string(where);
    NoteFailure(layout.note);
    g_layout = layout;
    return g_layout;
  }

  std::uint32_t vehicle[kPathAreas], ped[kPathAreas];
  asi::mem::ReadGuarded(layout.count_vehicle, vehicle, sizeof(vehicle));
  asi::mem::ReadGuarded(layout.count_ped, ped, sizeof(ped));
  for (int i = 0; i < kPathAreas; ++i)
    if (node_pointers[i] != 0) layout.ped_nodes_loaded += static_cast<int>(ped[i]);

  layout.valid = true;
  g_layout     = layout;
  g_resolved   = true;
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
  std::uint32_t pointer = 0, total = 0, vehicle = 0;
  if (!asi::mem::Read<std::uint32_t>(g_layout.nodes + area * 4, &pointer) ||
      pointer == 0)
    return false;
  if (!asi::mem::Read<std::uint32_t>(g_layout.count_all + area * 4, &total) ||
      index >= total)
    return false;
  asi::mem::Read<std::uint32_t>(g_layout.count_vehicle + area * 4, &vehicle);
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
  if (asi::mem::ReadGuarded(g_layout.count_all, total, sizeof(total)) != sizeof(total))
    return found;
  if (asi::mem::ReadGuarded(g_layout.count_vehicle, vehicle, sizeof(vehicle)) !=
      sizeof(vehicle))
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
