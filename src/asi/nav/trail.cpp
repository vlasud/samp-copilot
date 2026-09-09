#include "nav/trail.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "log.hpp"
#include "types.hpp"

namespace gtabot::nav {
namespace {

// Squares three quarters of a metre across. Coarser than a doorway is wide,
// which is the point: two squares joined means a body went from one to the
// other, doorway and all.
constexpr float kCell = 0.75f;
// Storeys. A server's interiors sit above one another at the same x and y.
constexpr float kLevel = 3.0f;
// Two squares are only joined by walking between them, and only when the
// step between them is one a person takes: neighbours, not a teleport.
constexpr float kStepAtMost = 2.0f;
// How near a route's ends have to be to something on record.
constexpr float kNearEnough = 4.0f;
constexpr int   kMaxVisited = 400000;
constexpr unsigned long long kSaveEveryMs = 60000;

struct Key {
  std::int32_t x = 0, y = 0, z = 0;
  bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct KeyHash {
  std::size_t operator()(const Key& k) const {
    // Three smallish integers into one word.
    std::size_t h = static_cast<std::uint32_t>(k.x) * 73856093u;
    h ^= static_cast<std::uint32_t>(k.y) * 19349663u;
    h ^= static_cast<std::uint32_t>(k.z) * 83492791u;
    return h;
  }
};

struct Square {
  std::vector<Key> to;      // squares he has stepped to from here
  int visits = 0;
};

std::mutex g_mutex;
std::unordered_map<Key, Square, KeyHash> g_map;
bool  g_loaded = false;
bool  g_dirty = false;
unsigned long long g_saved_ms = 0;
Key   g_last{};
bool  g_had_last = false;
int   g_routes_found = 0, g_routes_missed = 0;
std::string g_note = "nothing walked yet";

Key KeyOf(const Vec3& at) {
  return Key{static_cast<std::int32_t>(std::floor(at.x / kCell)),
             static_cast<std::int32_t>(std::floor(at.y / kCell)),
             static_cast<std::int32_t>(std::floor(at.z / kLevel))};
}

Vec3 Middle(const Key& k) {
  return Vec3{(k.x + 0.5f) * kCell, (k.y + 0.5f) * kCell, (k.z + 0.5f) * kLevel};
}

float Distance(const Key& a, const Key& b) {
  const float dx = static_cast<float>(a.x - b.x) * kCell;
  const float dy = static_cast<float>(a.y - b.y) * kCell;
  const float dz = static_cast<float>(a.z - b.z) * kLevel;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string Path() { return ModuleDirectory() + "bot.trail"; }

void Join(Square& square, const Key& to) {
  for (const Key& k : square.to)
    if (k == to) return;
  square.to.push_back(to);
}

void SaveLocked() {
  std::ofstream file(Path(), std::ios::trunc);
  if (!file) return;
  file << "gtabot trail 1\n";
  for (const auto& entry : g_map) {
    file << entry.first.x << ' ' << entry.first.y << ' ' << entry.first.z << ' '
         << entry.second.visits;
    for (const Key& to : entry.second.to)
      file << ' ' << to.x << ' ' << to.y << ' ' << to.z;
    file << '\n';
  }
  g_dirty = false;
  g_saved_ms = GetTickCount64();
}

}  // namespace

void TrailLoad() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_loaded) return;
  g_loaded = true;
  std::ifstream file(Path());
  if (!file) {
    g_note = "nothing learned yet - bot.trail will be written as he walks";
    return;
  }
  std::string head;
  std::getline(file, head);
  std::string line;
  std::size_t steps = 0;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    std::int32_t x = 0, y = 0, z = 0, visits = 0;
    const char* at = line.c_str();
    char* end = nullptr;
    x = std::strtol(at, &end, 10);
    y = std::strtol(end, &end, 10);
    z = std::strtol(end, &end, 10);
    visits = std::strtol(end, &end, 10);
    Square square;
    square.visits = visits;
    // The rest of the line is triples. strtol stops on the newline, so the
    // end of the line is where the numbers run out - checked before each
    // number rather than after, so a truncated file cannot invent one.
    for (;;) {
      while (*end == 32) ++end;   // spaces
      if (*end == 0) break;
      const std::int32_t tx = std::strtol(end, &end, 10);
      while (*end == 32) ++end;   // spaces
      if (*end == 0) break;
      const std::int32_t ty = std::strtol(end, &end, 10);
      while (*end == 32) ++end;   // spaces
      if (*end == 0) break;
      const std::int32_t tz = std::strtol(end, &end, 10);
      square.to.push_back(Key{tx, ty, tz});
      ++steps;
    }
    g_map.emplace(Key{x, y, z}, std::move(square));
  }
  char note[160];
  std::snprintf(note, sizeof(note),
                "%d squares and %d steps remembered from bot.trail",
                static_cast<int>(g_map.size()), static_cast<int>(steps));
  g_note = note;
  LOG_INFO("trail: {}", g_note);
}

void TrailVisit(const Vec3& at, bool on_ground) {
  if (!on_ground) return;
  TrailLoad();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_map.size() >= kMaxVisited) return;
  const Key key = KeyOf(at);
  Square& square = g_map[key];
  ++square.visits;
  if (g_had_last && !(g_last == key)) {
    // A step, not a teleport: a lift or a doorway that moved him across the
    // map is not a connection anybody can walk.
    if (Distance(g_last, key) <= kStepAtMost) {
      Join(g_map[g_last], key);
      Join(square, g_last);
      g_dirty = true;
    }
  }
  g_last = key;
  g_had_last = true;

  const unsigned long long now = GetTickCount64();
  if (g_dirty && now - g_saved_ms > kSaveEveryMs) SaveLocked();
}

bool TrailNearest(const Vec3& to, Vec3* at, float* away_m) {
  TrailLoad();
  std::lock_guard<std::mutex> lock(g_mutex);
  const Key want = KeyOf(to);
  float best = 1e9f;
  bool found = false;
  Key which{};
  for (const auto& entry : g_map) {
    if (entry.first.z != want.z) continue;      // a different storey
    const float away = Distance(entry.first, want);
    if (away < best) {
      best = away;
      which = entry.first;
      found = true;
    }
  }
  if (!found) return false;
  if (at != nullptr) *at = Middle(which);
  if (away_m != nullptr) *away_m = best;
  return true;
}

std::vector<Vec3> TrailRoute(const Vec3& from, const Vec3& to) {
  TrailLoad();
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<Vec3> out;
  if (g_map.empty()) {
    ++g_routes_missed;
    return out;
  }

  // The squares nearest each end, on the same storey.
  const Key want_from = KeyOf(from), want_to = KeyOf(to);
  Key start{}, goal{};
  float best_start = 1e9f, best_goal = 1e9f;
  for (const auto& entry : g_map) {
    if (entry.first.z == want_from.z) {
      const float away = Distance(entry.first, want_from);
      if (away < best_start) { best_start = away; start = entry.first; }
    }
    if (entry.first.z == want_to.z) {
      const float away = Distance(entry.first, want_to);
      if (away < best_goal) { best_goal = away; goal = entry.first; }
    }
  }
  if (best_start > kNearEnough || best_goal > kNearEnough) {
    ++g_routes_missed;
    return out;
  }

  // A* over what he has walked.
  struct Open { float f; Key at; };
  struct Worse {
    bool operator()(const Open& a, const Open& b) const { return a.f > b.f; }
  };
  std::priority_queue<Open, std::vector<Open>, Worse> open;
  std::unordered_map<Key, float, KeyHash> cost;
  std::unordered_map<Key, Key, KeyHash> came;
  open.push(Open{Distance(start, goal), start});
  cost[start] = 0.0f;
  bool arrived = false;
  int looked = 0;
  while (!open.empty() && looked < 200000) {
    const Key here = open.top().at;
    open.pop();
    ++looked;
    if (here == goal) { arrived = true; break; }
    const auto found = g_map.find(here);
    if (found == g_map.end()) continue;
    const float so_far = cost[here];
    for (const Key& next : found->second.to) {
      const float step = so_far + Distance(here, next);
      const auto known = cost.find(next);
      if (known != cost.end() && known->second <= step) continue;
      cost[next] = step;
      came[next] = here;
      open.push(Open{step + Distance(next, goal), next});
    }
  }
  if (!arrived) {
    ++g_routes_missed;
    return out;
  }

  for (Key at = goal;; ) {
    out.push_back(Middle(at));
    if (at == start) break;
    const auto back = came.find(at);
    if (back == came.end()) break;
    at = back->second;
  }
  std::reverse(out.begin(), out.end());
  ++g_routes_found;
  return out;
}

void TrailSave() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_dirty) SaveLocked();
}

TrailFacts TrailGet() {
  std::lock_guard<std::mutex> lock(g_mutex);
  TrailFacts facts;
  facts.squares = g_map.size();
  for (const auto& entry : g_map) facts.steps += entry.second.to.size();
  facts.routes_found = g_routes_found;
  facts.routes_missed = g_routes_missed;
  facts.loaded = g_loaded;
  facts.note = g_note;
  return facts;
}

}  // namespace gtabot::nav
