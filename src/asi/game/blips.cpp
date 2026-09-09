#include "game/blips.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

#include "game/exe.hpp"
#include "state/memory.hpp"

namespace gtabot::game {
namespace {

// The same array `MapMarker` indexes into: CRadar::ms_RadarTrace, 175 traces
// of 0x28 bytes. The offsets that module established by use are kept, and
// the rest of each trace goes out as bytes rather than being guessed at.
constexpr std::uint32_t kTargetBlip  = 0xBA6774;   // FrontEndMenuManager+0x2C
constexpr std::uint32_t kTraces      = 0xBA86F0;
constexpr std::uint32_t kTraceCount  = 175;
constexpr std::uint32_t kTraceSize   = 0x28;
constexpr std::uint32_t kTraceColour = 0x00;
constexpr std::uint32_t kTraceEntity = 0x04;
constexpr std::uint32_t kTracePos    = 0x08;
constexpr std::uint32_t kTraceCounter = 0x14;
constexpr std::uint32_t kTraceFlags   = 0x25;
constexpr std::uint32_t kTraceKind    = 0x26;
constexpr std::uint8_t  kTracking     = 0x02;

// What a destination from /gps looks like, established by watching one
// arrive rather than taken from any header. Three groups share this array
// and only one of them is a place the server is sending him:
//
//   the destination      colour 8, flags 0x03, byte 0x26 = 0x13
//   the city's own icons  colour 8, flags 0x07, byte 0x26 = 0x13
//   businesses and cars   other colours, byte 0x26 = 0x06 or 0x0A, and they
//                         come and go by themselves as the world streams
//
// Without the last line the newest mark was whichever shop had just drifted
// into range, and the warehouse he had asked for lasted one turn.
constexpr std::uint32_t kMarkColour = 8;
constexpr std::uint8_t  kMarkFlags  = 0x03;
constexpr std::uint8_t  kMarkKind   = 0x13;

bool LooksLikeADestination(const Blip& one) {
  return one.entity == 0 && one.colour == kMarkColour &&
         one.flags == kMarkFlags && one.kind == kMarkKind;
}

// What was on the radar last time, as index and that slot's counter - the
// pair changes when a slot is reused, which is what makes a reused slot a
// new mark rather than the same one.
std::set<std::pair<int, std::uint16_t>> g_seen;
bool g_looked_before = false;
Blip g_last;
bool g_have_last = false;

// A blip with no position is an empty slot or one that follows an entity.
bool Somewhere(const float (&pos)[3]) {
  for (float one : pos)
    if (!std::isfinite(one) || std::fabs(one) > 20000.0f) return false;
  return !(pos[0] == 0 && pos[1] == 0 && pos[2] == 0);
}

}  // namespace

std::vector<Blip> BlipsNow(const Vec3& from) {
  std::vector<Blip> found;
  std::set<std::pair<int, std::uint16_t>> now;
  for (std::uint32_t i = 0; i < kTraceCount; ++i) {
    const std::uintptr_t trace = At(kTraces) + i * kTraceSize;
    std::uint8_t raw[kTraceSize] = {};
    bool ok = true;
    for (std::uint32_t b = 0; b < kTraceSize && ok; ++b)
      ok = asi::mem::Read<std::uint8_t>(trace + b, &raw[b]);
    if (!ok) continue;

    float pos[3] = {0, 0, 0};
    std::memcpy(pos, raw + kTracePos, sizeof(pos));
    if (!Somewhere(pos)) continue;

    Blip one;
    one.index = static_cast<int>(i);
    one.at = Vec3{pos[0], pos[1], pos[2]};
    const float dx = pos[0] - from.x, dy = pos[1] - from.y;
    one.away_m = std::sqrt(dx * dx + dy * dy);
    std::memcpy(&one.colour, raw + kTraceColour, sizeof(one.colour));
    std::memcpy(&one.entity, raw + kTraceEntity, sizeof(one.entity));
    std::memcpy(&one.counter, raw + kTraceCounter, sizeof(one.counter));
    one.flags = raw[kTraceFlags];
    one.kind = raw[kTraceKind];
    one.tracking = (one.flags & kTracking) != 0;

    char hex[kTraceSize * 3 + 1];
    for (std::uint32_t b = 0; b < kTraceSize; ++b)
      std::snprintf(hex + b * 3, 4, "%02X ", raw[b]);
    one.bytes = hex;

    const auto who = std::make_pair(one.index, one.counter);
    if (g_looked_before && g_seen.find(who) == g_seen.end()) {
      one.fresh = true;
      if (LooksLikeADestination(one)) {
        g_last = one;
        g_have_last = true;
      }
    }
    now.insert(who);
    found.push_back(std::move(one));
  }
  g_seen.swap(now);
  g_looked_before = true;
  // A mark that has gone is no longer anywhere to go.
  if (g_have_last &&
      g_seen.find(std::make_pair(g_last.index, g_last.counter)) == g_seen.end())
    g_have_last = false;
  return found;
}

bool AppearedLast(Blip* out) {
  if (!g_have_last || out == nullptr) return false;
  *out = g_last;
  return true;
}

int WaypointIndex() {
  std::int32_t handle = 0;
  if (!asi::mem::Read<std::int32_t>(At(kTargetBlip), &handle)) return -1;
  if (handle <= 0) return -1;
  const std::uint32_t index = static_cast<std::uint32_t>(handle) & 0xFFFF;
  return index < kTraceCount ? static_cast<int>(index) : -1;
}

}  // namespace gtabot::game
