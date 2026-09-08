#include "samp/labels.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "log.hpp"
#include "samp/version.hpp"
#include "state/memory.hpp"
#include "types.hpp"

namespace gtabot::samp {
namespace {

// What an entry has to look like: a pointer to a string, then somewhere in
// the world. The stride is not assumed - a build that adds a field to the
// end would move it - so the run is measured at each of the plausible ones
// and the longest wins.
constexpr std::uint32_t kStrides[] = {0x18, 0x1C, 0x20, 0x24, 0x28};
constexpr std::uint32_t kTextAt = 0x00;
constexpr std::uint32_t kPosAt  = 0x08;
constexpr std::uint32_t kDistanceAt = 0x14;
constexpr int   kRunToBelieve = 8;       // consecutive entries
// What tells the server's own writing apart from any other table of strings
// with numbers beside it: it colours it, and on a Russian server it is
// written in Russian.
constexpr int   kMarkedToBelieve = 3;
// And they stand in different places; a table of strings with one position
// repeated is something else entirely.
constexpr int   kSpreadToBelieve = 2;
// And the strongest sign of all: a sign a person can read is a sign near
// enough to be read. The game's own string tables sit at the middle of the
// map with the numbers beside them reading as nought.
constexpr float kNearPlayer = 250.0f;
constexpr int   kNearToBelieve = 1;
constexpr int   kMaxEntries   = 4096;
constexpr float kWorldEdge    = 4000.0f;
constexpr std::size_t kMaxTextBytes = 400;

std::vector<std::pair<std::uintptr_t, std::uintptr_t>> g_committed;

bool Committed(std::uint32_t at) {
  std::size_t low = 0, high = g_committed.size();
  while (low < high) {
    const std::size_t middle = (low + high) / 2;
    if (at < g_committed[middle].first) high = middle;
    else if (at >= g_committed[middle].second) low = middle + 1;
    else return true;
  }
  return false;
}

std::uintptr_t g_table = 0;
std::uint32_t  g_stride = 0;
int  g_count = 0;
bool g_looked = false;
std::string g_note = "not looked for yet";

bool Printable(unsigned char c) {
  return c >= 0x20 || c == 0x09;   // the client colours text with {RRGGBB}
}

// A string a person could be meant to read: some length, no control bytes.
bool ReadableText(std::uint32_t at, std::string* out) {
  if (at < 0x10000 || at > 0x7FFF0000) return false;
  if (!g_committed.empty() && !Committed(at)) return false;
  char buffer[kMaxTextBytes + 1] = {};
  const std::size_t got = asi::mem::ReadGuarded(at, buffer, kMaxTextBytes);
  if (got < 2) return false;
  std::size_t length = 0;
  while (length < got && buffer[length] != '\0') {
    if (!Printable(static_cast<unsigned char>(buffer[length]))) return false;
    ++length;
  }
  if (length < 2 || length >= got) return false;   // no terminator in reach
  if (out != nullptr) out->assign(buffer, length);
  return true;
}

bool PlausibleEntry(std::uintptr_t entry, std::string* text, Vec3* at) {
  // Guarded copies, never a query: this runs over millions of candidates and
  // one system call apiece would take the afternoon.
  unsigned char raw[0x18];
  if (asi::mem::ReadGuarded(entry, raw, sizeof(raw)) != sizeof(raw)) return false;
  std::uint32_t pointer = 0;
  std::memcpy(&pointer, raw + kTextAt, 4);
  if (!ReadableText(pointer, text)) return false;
  float x = 0, y = 0, z = 0;
  std::memcpy(&x, raw + kPosAt + 0, 4);
  std::memcpy(&y, raw + kPosAt + 4, 4);
  std::memcpy(&z, raw + kPosAt + 8, 4);
  if (!(x == x) || !(y == y) || !(z == z)) return false;
  if (std::fabs(x) > kWorldEdge || std::fabs(y) > kWorldEdge ||
      std::fabs(z) > kWorldEdge)
    return false;
  // Not at the middle of the map: the game's own string tables sit next to
  // numbers that read as a position of nought, and there is nothing at the
  // origin of San Andreas but water.
  if (std::fabs(x) < 3.0f && std::fabs(y) < 3.0f) return false;
  if (z < -200.0f || z > 2000.0f) return false;
  // How far away the server lets it be read from. Anything is possible in
  // principle; in practice it is metres, not millions.
  float distance = 0;
  std::memcpy(&distance, raw + kDistanceAt, 4);
  if (!(distance > 0.5f && distance < 2000.0f)) return false;
  if (at != nullptr) *at = Vec3{x, y, z};
  return true;
}

// How many entries in a row hold up from here, and how many of those read
// like something a server wrote for a person: a colour code, or Cyrillic.
Vec3 g_near_to{};

int RunFrom(std::uintptr_t start, std::uint32_t stride, int* marked, int* spread,
            int* near_player) {
  int run = 0;
  if (marked != nullptr) *marked = 0;
  if (spread != nullptr) *spread = 0;
  if (near_player != nullptr) *near_player = 0;
  std::string text;
  Vec3 at{};
  Vec3 first{};
  while (run < kMaxEntries &&
         PlausibleEntry(start + static_cast<std::uint32_t>(run) * stride,
                        &text, &at)) {
    if (run == 0) first = at;
    else if (spread != nullptr) {
      const float dx = at.x - first.x, dy = at.y - first.y;
      if (std::sqrt(dx * dx + dy * dy) > 10.0f) ++*spread;
    }
    if (near_player != nullptr) {
      const float dx = at.x - g_near_to.x, dy = at.y - g_near_to.y;
      if (std::sqrt(dx * dx + dy * dy) < kNearPlayer) ++*near_player;
    }
    if (marked != nullptr) {
      bool looks_written = text.find('{') != std::string::npos;
      if (!looks_written)
        for (unsigned char c : text)
          if (c >= 0x80) { looks_written = true; break; }
      if (looks_written) ++*marked;
    }
    ++run;
  }
  return run;
}

// The client's own writable memory, walked once, looking for that shape.
void Find(const Vec3& near_to) {
  // Found once, kept. Not found, looked for again in a while: the server
  // hangs these up as the character comes near them, so an empty street is
  // an answer about the street rather than about the client.
  static unsigned long long looked_ms = 0;
  const unsigned long long now = GetTickCount64();
  if (g_table != 0) return;
  if (g_looked && now - looked_ms < 20000) return;
  g_looked = true;
  looked_ms = now;
  g_near_to = near_to;
  const Client client = Detect();
  if (client.base == 0) {
    g_note = "the client is not loaded";
    return;
  }

  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  auto address = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
  const auto ceiling = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
  int best_run = 0, best_marked = 0;
  std::uintptr_t best_table = 0;
  std::uint32_t best_stride = 0;
  std::size_t regions = 0;

  // Every committed page, once, so that "does this point at anything?" is a
  // comparison rather than a system call.
  g_committed.clear();
  {
    std::uintptr_t walk = address;
    while (walk < ceiling) {
      MEMORY_BASIC_INFORMATION where{};
      if (VirtualQuery(reinterpret_cast<LPCVOID>(walk), &where, sizeof(where)) == 0)
        break;
      const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(where.BaseAddress);
      const std::uintptr_t end = base + where.RegionSize;
      if (where.State == MEM_COMMIT && (where.Protect & PAGE_GUARD) == 0 &&
          (where.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) != 0)
        g_committed.push_back({base, end});
      if (end <= walk) break;
      walk = end;
    }
  }

  while (address < ceiling) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) == 0)
      break;
    const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) +
                                region.RegionSize;
    const bool writable = region.State == MEM_COMMIT &&
                          (region.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0 &&
                          (region.Protect & PAGE_GUARD) == 0;
    // A table of a few thousand entries lives in a region of its own size or
    // larger; the tiny ones cannot hold one and the enormous ones are the
    // game's own heaps.
    if (writable && region.RegionSize >= 0x8000 && region.RegionSize <= 0x2000000) {
      ++regions;
      // The region in one go, then read out of the copy: the same scan
      // through the process a dword at a time is a system call each and
      // never finishes.
      const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
      std::vector<unsigned char> copy(region.RegionSize);
      const std::size_t got = asi::mem::ReadGuarded(base, copy.data(), copy.size());
      if (got < 0x8000) { address = next; continue; }
      const std::size_t usable = got > 0x400 ? got - 0x400 : 0;
      for (std::size_t offset = 0; offset + 4 <= usable; offset += 4) {
        const std::uintptr_t at = base + offset;
        std::uint32_t pointer = 0;
        std::memcpy(&pointer, copy.data() + offset, 4);
        if (pointer < 0x10000 || pointer > 0x7FFF0000) continue;
        if (!ReadableText(pointer, nullptr)) continue;
        for (std::uint32_t stride : kStrides) {
          int marked = 0, spread = 0, near_player = 0;
          const int run = RunFrom(at, stride, &marked, &spread, &near_player);
          // Standing apart from one another, as things hung about a city do,
          // and at least one of them near enough to be read from here.
          if (run < kRunToBelieve || marked < kMarkedToBelieve ||
              spread < kSpreadToBelieve || near_player < kNearToBelieve)
            continue;
          // The table with the most of the server's own writing in it.
          if (marked > best_marked || (marked == best_marked && run > best_run)) {
            best_run = run;
            best_marked = marked;
            best_table = at;
            best_stride = stride;
          }
        }
        // Past the start of a run there is no point walking into it.
        if (best_table == at && best_run > 1)
          offset += static_cast<std::size_t>(best_run - 1) * best_stride;
      }
    }
    if (next <= address) break;
    address = next;
  }

  if (best_run < kRunToBelieve || best_marked < kMarkedToBelieve) {
    g_note = "no table of three-dimensional text was found (best run " +
             std::to_string(best_run) + ", " + std::to_string(best_marked) +
             " of them written for a person, over " + std::to_string(regions) +
             " regions)";
    LOG_WARN("labels: {}", g_note);
    return;
  }
  g_table = best_table;
  g_stride = best_stride;
  g_count = best_run;
  g_note = "found " + std::to_string(best_run) + " at 0x" +
           [&] {
             char text[16];
             std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(best_table));
             return std::string(text);
           }() +
           ", " + std::to_string(best_stride) + " bytes each, " +
           std::to_string(best_marked) + " written for a person";
  LOG_INFO("labels: {}", g_note);
}

}  // namespace

std::vector<Label> LabelsNear(const Vec3& at, float radius, std::size_t max) {
  Find(at);
  std::vector<Label> found;
  if (g_table == 0) return found;
  const float radius_squared = radius * radius;
  for (int i = 0; i < g_count; ++i) {
    const std::uintptr_t entry = g_table + static_cast<std::uint32_t>(i) * g_stride;
    Label label;
    if (!PlausibleEntry(entry, &label.text, &label.at)) continue;
    const float dx = label.at.x - at.x, dy = label.at.y - at.y,
                dz = label.at.z - at.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > radius_squared) continue;
    label.away_m = std::sqrt(d2);
    asi::mem::Read<float>(entry + kDistanceAt, &label.draw_distance);
    label.text = ToUtf8(label.text);
    found.push_back(std::move(label));
  }
  std::sort(found.begin(), found.end(),
            [](const Label& a, const Label& b) { return a.away_m < b.away_m; });
  if (found.size() > max) found.resize(max);
  return found;
}

std::vector<Label> LabelsAny(const Vec3& from, std::size_t max) {
  Find(from);
  std::vector<Label> found;
  if (g_table == 0) return found;
  for (int i = 0; i < g_count && found.size() < max; ++i) {
    const std::uintptr_t entry = g_table + static_cast<std::uint32_t>(i) * g_stride;
    Label label;
    if (!PlausibleEntry(entry, &label.text, &label.at)) continue;
    const float dx = label.at.x - from.x, dy = label.at.y - from.y,
                dz = label.at.z - from.z;
    label.away_m = std::sqrt(dx * dx + dy * dy + dz * dz);

    label.text = ToUtf8(label.text);
    found.push_back(std::move(label));
  }
  return found;
}

std::string LabelsNote() { return g_note; }

}  // namespace gtabot::samp
