#include "samp/checkpoints.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include "samp/version.hpp"
#include "state/memory.hpp"

namespace gtabot::samp {
namespace {

// CGame, packed, from the client's own structures
// (github.com/BlastHackNet/SAMP-API, 0.3.7-R1):
//
//   CAudio*  +0x00   CCamera* +0x04   CPed* +0x08
//   checkpoint       position +0x0C  size +0x18  enabled +0x24  handle +0x28
//   racing checkpoint  here +0x2C  next +0x38  size +0x44  type +0x48
//                      enabled +0x49  marker +0x4D  handle +0x51
//   cursor mode      +0x55
//
// That last one is the check. The cursor mode at +0x55 was established in
// this project long ago and for quite another reason, so if it still reads
// as a cursor mode - nought to three - everything in front of it is where
// the header says it is.
constexpr std::uint32_t kGameRva = 0x21A10C;

constexpr std::uint32_t kCheckpointAt      = 0x0C;
constexpr std::uint32_t kCheckpointSize    = 0x18;
constexpr std::uint32_t kCheckpointEnabled = 0x24;
constexpr std::uint32_t kRaceAt      = 0x2C;
constexpr std::uint32_t kRaceNext    = 0x38;
constexpr std::uint32_t kRaceSize    = 0x44;
constexpr std::uint32_t kRaceType    = 0x48;
constexpr std::uint32_t kRaceEnabled = 0x49;
constexpr std::uint32_t kCursorMode  = 0x55;

std::string g_note = "not looked at yet";

std::uintptr_t Game() {
  const Client client = Detect();
  if (client.base == 0) {
    g_note = "the SA-MP client is not loaded";
    return 0;
  }
  std::uint32_t game = 0;
  if (!asi::mem::Read<std::uint32_t>(client.base + kGameRva, &game) || game == 0) {
    g_note = "CGame is not where this build keeps it";
    return 0;
  }
  if (!asi::mem::IsReadable(game, 0x60)) {
    g_note = "CGame does not read";
    return 0;
  }
  std::uint32_t cursor = 0;
  if (!asi::mem::Read<std::uint32_t>(game + kCursorMode, &cursor) || cursor > 3) {
    // The one field in here whose value is known independently. If it is not
    // a cursor mode, this is not CGame and nothing else should be believed.
    char said[128];
    std::snprintf(said, sizeof(said),
                  "the field that should be the cursor mode reads %u, so this "
                  "is not CGame", static_cast<unsigned>(cursor));
    g_note = said;
    return 0;
  }
  return game;
}

Vec3 ReadVec(std::uintptr_t at) {
  Vec3 out;
  asi::mem::Read<float>(at, &out.x);
  asi::mem::Read<float>(at + 4, &out.y);
  asi::mem::Read<float>(at + 8, &out.z);
  return out;
}

float Away(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

bool Plausible(const Vec3& at) {
  return std::fabs(at.x) < 20000.0f && std::fabs(at.y) < 20000.0f &&
         std::fabs(at.z) < 20000.0f && !(at.x == 0 && at.y == 0 && at.z == 0);
}

}  // namespace

Checkpoint CheckpointNow(const Vec3& from) {
  Checkpoint out;
  const std::uintptr_t game = Game();
  if (game == 0) return out;
  std::uint32_t enabled = 0;
  if (!asi::mem::Read<std::uint32_t>(game + kCheckpointEnabled, &enabled) ||
      enabled == 0) {
    g_note = "no checkpoint is being shown";
    return out;
  }
  const Vec3 at = ReadVec(game + kCheckpointAt);
  if (!Plausible(at)) {
    g_note = "the checkpoint reads as nowhere";
    return out;
  }
  out.shown = true;
  out.at = at;
  const Vec3 size = ReadVec(game + kCheckpointSize);
  out.size = size.x;
  out.away_m = Away(at, from);
  g_note = "a checkpoint is being shown";
  return out;
}

RaceCheckpoint RaceCheckpointNow(const Vec3& from) {
  RaceCheckpoint out;
  const std::uintptr_t game = Game();
  if (game == 0) return out;
  std::uint32_t enabled = 0;
  if (!asi::mem::Read<std::uint32_t>(game + kRaceEnabled, &enabled) ||
      enabled == 0)
    return out;
  const Vec3 at = ReadVec(game + kRaceAt);
  if (!Plausible(at)) return out;
  out.shown = true;
  out.at = at;
  out.next = ReadVec(game + kRaceNext);
  asi::mem::Read<float>(game + kRaceSize, &out.size);
  std::uint8_t type = 0;
  if (asi::mem::Read<std::uint8_t>(game + kRaceType, &type)) out.type = type;
  out.away_m = Away(at, from);
  return out;
}

std::string CheckpointsNote() { return g_note; }

}  // namespace gtabot::samp
