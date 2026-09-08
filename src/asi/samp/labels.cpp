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

// Where the client keeps them, from the SA-MP structures BlastHack published
// (github.com/BlastHackNet/SAMP-API, 0.3.7-R1). Everything in the client is
// packed, so each offset is the plain sum of what comes before it:
//
//   CNetGame: thirty-two bytes of padding, the host address and the hostname
//   as 257-byte strings, three flags, the port, the LAN flag, a hundred map
//   icon references, the game state at +0x3BD - which is the offset this
//   module already used and had checked against a live client, so the sum
//   holds - the last connect attempt, the settings, the RakNet client, and
//   the pools at +0x3CD.
//
//   Pools: actor, object, gang zone, then the labels at +0x0C.
//
//   CLabelPool: two thousand and forty-eight entries of twenty-nine bytes,
//   followed by as many flags saying which of them are in use.
//
// Looking for that shape instead of reading it found the game's own string
// tables three times over. Twenty-nine bytes is not a stride anybody guesses.
constexpr std::uint32_t kNetGameRva  = 0x21A0F8;
constexpr std::uint32_t kPoolsAt     = 0x3CD;
constexpr std::uint32_t kLabelPoolAt = 0x0C;
constexpr int           kMaxLabels   = 2048;
constexpr std::uint32_t kEntryBytes  = 29;
constexpr std::uint32_t kInUseAt     = kMaxLabels * kEntryBytes;   // 59392
// Inside one entry.
constexpr std::uint32_t kText            = 0x00;
constexpr std::uint32_t kPosition        = 0x08;
constexpr std::uint32_t kDistance        = 0x14;
constexpr std::uint32_t kBehindWalls     = 0x18;
constexpr std::uint32_t kAttachedPlayer  = 0x19;
constexpr std::uint32_t kAttachedVehicle = 0x1B;

constexpr std::size_t kMaxTextBytes = 512;
constexpr float kWorldEdge = 4000.0f;

std::string g_note = "not looked at yet";

std::uintptr_t LabelPool() {
  const Client client = Detect();
  if (client.base == 0) return 0;
  std::uint32_t netgame = 0;
  if (!asi::mem::Read<std::uint32_t>(client.base + kNetGameRva, &netgame) ||
      netgame == 0)
    return 0;
  std::uint32_t pools = 0;
  if (!asi::mem::Read<std::uint32_t>(netgame + kPoolsAt, &pools) || pools == 0)
    return 0;
  std::uint32_t labels = 0;
  if (!asi::mem::Read<std::uint32_t>(pools + kLabelPoolAt, &labels) || labels == 0)
    return 0;
  if (!asi::mem::IsReadable(labels, kInUseAt + kMaxLabels * 4)) return 0;
  return labels;
}

std::string ReadText(std::uint32_t at) {
  if (at < 0x10000) return {};
  char buffer[kMaxTextBytes + 1] = {};
  const std::size_t got = asi::mem::ReadGuarded(at, buffer, kMaxTextBytes);
  if (got == 0) return {};
  return std::string(buffer, ::strnlen(buffer, got));
}

}  // namespace

std::vector<Label> LabelsNear(const Vec3& at, float radius, std::size_t max) {
  std::vector<Label> found;
  const std::uintptr_t pool = LabelPool();
  if (pool == 0) {
    g_note = "the client's label pool is not where this build keeps it";
    return found;
  }
  const float radius_squared = radius * radius;
  int in_use = 0;
  for (int i = 0; i < kMaxLabels; ++i) {
    std::uint32_t used = 0;
    if (!asi::mem::Read<std::uint32_t>(pool + kInUseAt + i * 4, &used) || used == 0)
      continue;
    ++in_use;
    const std::uintptr_t entry = pool + static_cast<std::uint32_t>(i) * kEntryBytes;
    unsigned char raw[kEntryBytes];
    if (asi::mem::ReadGuarded(entry, raw, sizeof(raw)) != sizeof(raw)) continue;

    Label label;
    std::uint32_t text = 0;
    std::memcpy(&text, raw + kText, 4);
    label.text = ToUtf8(ReadText(text));
    if (label.text.empty()) continue;
    std::memcpy(&label.at.x, raw + kPosition + 0, 4);
    std::memcpy(&label.at.y, raw + kPosition + 4, 4);
    std::memcpy(&label.at.z, raw + kPosition + 8, 4);
    std::memcpy(&label.draw_distance, raw + kDistance, 4);
    if (!(label.at.x == label.at.x) || std::fabs(label.at.x) > kWorldEdge ||
        std::fabs(label.at.y) > kWorldEdge)
      continue;
    std::memcpy(&label.attached_to_player, raw + kAttachedPlayer, 2);
    std::memcpy(&label.attached_to_vehicle, raw + kAttachedVehicle, 2);
    label.behind_walls = raw[kBehindWalls] != 0;
    label.id = i;

    // One hung on a player or a car travels with it, and the position in its
    // own entry is not where it is; those are kept whatever the distance
    // says, and marked by a distance of less than nothing.
    const bool attached = label.attached_to_player != 0xFFFF ||
                          label.attached_to_vehicle != 0xFFFF;
    const float dx = label.at.x - at.x, dy = label.at.y - at.y,
                dz = label.at.z - at.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (!attached && d2 > radius_squared) continue;
    label.away_m = attached ? -1.0f : std::sqrt(d2);
    found.push_back(std::move(label));
  }
  g_note = std::to_string(in_use) + " of the client's " +
           std::to_string(kMaxLabels) + " label slots are in use";
  std::sort(found.begin(), found.end(),
            [](const Label& a, const Label& b) { return a.away_m < b.away_m; });
  if (found.size() > max) found.resize(max);
  return found;
}

std::vector<Label> LabelsAny(const Vec3& from, std::size_t max) {
  return LabelsNear(from, 100000.0f, max);
}

std::string LabelsNote() { return g_note; }

}  // namespace gtabot::samp
