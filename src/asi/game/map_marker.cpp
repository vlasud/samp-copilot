#include "game/map_marker.hpp"

#include <cstdint>

#include "game/exe.hpp"
#include "state/memory.hpp"

namespace gtabot::game {
namespace {

// FrontEndMenuManager (0xBA6748) + 0x2C: m_nTargetBlipIndex. The map code
// stores CRadar::SetCoordBlip(BLIP_COORD, ...) there and clears it to zero,
// so zero is "no marker". A handle is the trace index in the low word and
// that trace's counter in the high word.
constexpr std::uint32_t kTargetBlip = 0xBA6774;
// CRadar::ms_RadarTrace: 175 traces of 0x28 bytes.
constexpr std::uint32_t kTraces      = 0xBA86F0;
constexpr std::uint32_t kTraceCount  = 175;
constexpr std::uint32_t kTraceSize   = 0x28;
constexpr std::uint32_t kTracePos     = 0x08;   // CVector
constexpr std::uint32_t kTraceCounter = 0x14;   // uint16
constexpr std::uint32_t kTraceFlags   = 0x25;   // bit 1: m_bTrackingBlip
constexpr std::uint8_t  kTracking     = 0x02;

}  // namespace

bool MapMarker(Vec3* out) {
  std::int32_t handle = 0;
  if (!asi::mem::Read<std::int32_t>(At(kTargetBlip), &handle)) return false;
  if (handle <= 0) return false;
  const std::uint32_t index   = static_cast<std::uint32_t>(handle) & 0xFFFF;
  const std::uint32_t counter = (static_cast<std::uint32_t>(handle) >> 16) & 0xFFFF;
  if (index >= kTraceCount) return false;

  const std::uintptr_t trace = At(kTraces) + index * kTraceSize;
  std::uint16_t have = 0;
  std::uint8_t  flags = 0;
  float         pos[3] = {0, 0, 0};
  if (!asi::mem::Read<std::uint16_t>(trace + kTraceCounter, &have)) return false;
  if (!asi::mem::Read<std::uint8_t>(trace + kTraceFlags, &flags)) return false;
  for (std::uint32_t i = 0; i < 3; ++i)
    if (!asi::mem::Read<float>(trace + kTracePos + i * 4, &pos[i])) return false;
  // A stale handle names a trace that has since been reused or freed.
  if (have != counter || (flags & kTracking) == 0) return false;
  *out = Vec3{pos[0], pos[1], pos[2]};
  return true;
}

}  // namespace gtabot::game
