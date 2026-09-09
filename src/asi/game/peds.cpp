#include "game/peds.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "state/memory.hpp"

namespace gtabot::game {
namespace {

// CPools::ms_pPedPool. The pool is the same shape as every other one in this
// game: an array of objects, a byte per slot saying whether it is in use, and
// the count. A slot is free when the top bit of its byte is set.
constexpr std::uint32_t kPedPool   = 0xB74490;
constexpr std::uint32_t kPoolItems = 0x00;
constexpr std::uint32_t kPoolBytes = 0x04;
constexpr std::uint32_t kPoolSize  = 0x08;
constexpr std::uint32_t kPedStride = 0x7C4;

constexpr std::uint32_t kEntityMatrix   = 0x14;
constexpr std::uint32_t kEntityPosition = 0x04;
constexpr std::uint32_t kMatrixForward  = 0x10;
constexpr std::uint32_t kMatrixPosition = 0x30;
// CPed::m_nPedType, which is 0 for the player and something else for
// everybody the game or the server made.
constexpr std::uint32_t kPedType = 0x484;
// CEntity::m_nModelIndex - for a ped, the skin.
constexpr std::uint32_t kEntityModel = 0x22;

std::string g_note = "not looked at yet";

bool Plausible(float x, float y, float z) {
  return std::fabs(x) < 20000.0f && std::fabs(y) < 20000.0f &&
         std::fabs(z) < 20000.0f && !(x == 0.0f && y == 0.0f && z == 0.0f);
}

}  // namespace

std::vector<Ped> PedsNear(const Vec3& at, float radius, std::size_t max,
                          std::uintptr_t skip) {
  std::vector<Ped> found;
  std::uint32_t pool = 0;
  if (!asi::mem::Read<std::uint32_t>(kPedPool, &pool) || pool == 0) {
    g_note = "the game's ped pool is not there yet";
    return found;
  }
  std::uint32_t items = 0, bytes = 0;
  std::int32_t  size = 0;
  const bool read_all =
      asi::mem::Read<std::uint32_t>(pool + kPoolItems, &items) &&
      asi::mem::Read<std::uint32_t>(pool + kPoolBytes, &bytes) &&
      asi::mem::Read<std::int32_t>(pool + kPoolSize, &size);
  // A server raises the ped limit, so the count is not the game's own 140;
  // what makes it a pool is two pointers and a count that is not absurd.
  if (!read_all || items == 0 || bytes == 0 || size <= 0 || size > 20000) {
    char said[160];
    std::snprintf(said, sizeof(said),
                  "the pool at 0x%08X reads items=0x%08X bytes=0x%08X "
                  "size=%d, which is not a pool",
                  static_cast<unsigned>(pool), static_cast<unsigned>(items),
                  static_cast<unsigned>(bytes), size);
    g_note = said;
    return found;
  }

  int in_use = 0;
  for (std::int32_t slot = 0; slot < size; ++slot) {
    std::uint8_t flags = 0;
    if (!asi::mem::Read<std::uint8_t>(bytes + slot, &flags)) continue;
    if ((flags & 0x80) != 0) continue;             // free
    ++in_use;
    const std::uintptr_t ped = items + static_cast<std::uint32_t>(slot) * kPedStride;
    if (ped == skip) continue;

    Ped who;
    who.at_address = ped;

    std::uint32_t matrix = 0;
    float x = 0, y = 0, z = 0, fx = 0, fy = 1.0f;
    if (asi::mem::Read<std::uint32_t>(ped + kEntityMatrix, &matrix) &&
        matrix != 0 &&
        asi::mem::Read<float>(matrix + kMatrixPosition, &x) &&
        asi::mem::Read<float>(matrix + kMatrixPosition + 4, &y) &&
        asi::mem::Read<float>(matrix + kMatrixPosition + 8, &z)) {
      asi::mem::Read<float>(matrix + kMatrixForward, &fx);
      asi::mem::Read<float>(matrix + kMatrixForward + 4, &fy);
    } else if (!asi::mem::Read<float>(ped + kEntityPosition, &x) ||
               !asi::mem::Read<float>(ped + kEntityPosition + 4, &y) ||
               !asi::mem::Read<float>(ped + kEntityPosition + 8, &z)) {
      continue;
    }
    if (!Plausible(x, y, z)) continue;

    const float dx = x - at.x, dy = y - at.y;
    const float away = std::sqrt(dx * dx + dy * dy);
    if (away > radius) continue;

    who.position = Vec3{x, y, z};
    who.heading = std::atan2(fy, fx);
    who.away_m = away;
    std::uint32_t type = 0;
    if (asi::mem::Read<std::uint32_t>(ped + kPedType, &type)) who.is_player = type == 0;
    std::int16_t model = -1;
    if (asi::mem::Read<std::int16_t>(ped + kEntityModel, &model) && model >= 0)
      who.skin = model;
    found.push_back(who);
  }

  std::sort(found.begin(), found.end(),
            [](const Ped& a, const Ped& b) { return a.away_m < b.away_m; });
  if (found.size() > max) found.resize(max);
  g_note = std::to_string(in_use) + " of the pool's " + std::to_string(size) +
           " ped slots are in use";
  return found;
}

Vec3 InFrontOf(const Ped& who, float away) {
  return Vec3{who.position.x + std::cos(who.heading) * away,
              who.position.y + std::sin(who.heading) * away, who.position.z};
}

std::string PedsNote() { return g_note; }

}  // namespace gtabot::game
