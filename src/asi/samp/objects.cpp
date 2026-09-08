#include "samp/objects.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.hpp"
#include "samp/version.hpp"
#include "state/memory.hpp"
#include "types.hpp"

namespace gtabot::samp {
namespace {

// Summed from the client's own structures (github.com/BlastHackNet/SAMP-API,
// 0.3.7-R1), which are packed throughout, so each offset is what comes
// before it and nothing else:
//
//   CEntity is a vtable, sixty bytes of padding, the game entity and its
//   handle: seventy-two bytes. CObject follows it with its model, draw
//   distance, position, rotation, what it is attached to, a matrix and two
//   stretches of padding, which puts its materials at +0x217.
//
//   ObjectMaterial: sixteen sprites, sixteen colours, sixty-eight bytes of
//   padding, then the sixteen material types at +0xC4 - two means text -
//   sixteen flags, sixteen blocks of text settings of two hundred and
//   fifteen bytes each, and then the sixteen text pointers at +0xEB4.
//
// A sum of that length is worth doubting, so it is checked rather than
// trusted: when it finds nothing anywhere, one object is searched for the
// pointer instead and the offset it really sits at goes in the log.
constexpr std::uint32_t kNetGameRva   = 0x21A0F8;
constexpr std::uint32_t kPoolsAt      = 0x3CD;
constexpr std::uint32_t kObjectPoolAt = 0x04;

constexpr int kMaxObjects = 1000;
constexpr std::uint32_t kLargestId = 0x00;
constexpr std::uint32_t kInUseAt   = 0x04;
constexpr std::uint32_t kPointersAt = kInUseAt + kMaxObjects * 4;   // 0xFA4

constexpr std::uint32_t kModelAt    = 0x4E;
constexpr std::uint32_t kPositionAt = 0x5C;
constexpr std::uint32_t kMaterialAt = 0x217;
constexpr std::uint32_t kTypeAt     = kMaterialAt + 0x0C4;
constexpr std::uint32_t kTextAt     = kMaterialAt + 0xEB4;
constexpr std::uint32_t kSettingsAt = kMaterialAt + 0x144;
constexpr std::uint32_t kSettingsBytes = 215;
constexpr std::uint32_t kFontAt     = 139;    // inside one block of settings
constexpr int kMaterials = 16;
constexpr int kTypeText  = 2;

constexpr std::size_t kMaxTextBytes = 800;
constexpr float kWorldEdge = 4000.0f;

std::string g_note = "not looked at yet";
bool g_calibrated = false;

std::uintptr_t Pool() {
  const Client client = Detect();
  if (client.base == 0) return 0;
  std::uint32_t netgame = 0;
  if (!asi::mem::Read<std::uint32_t>(client.base + kNetGameRva, &netgame) ||
      netgame == 0)
    return 0;
  std::uint32_t pools = 0;
  if (!asi::mem::Read<std::uint32_t>(netgame + kPoolsAt, &pools) || pools == 0)
    return 0;
  std::uint32_t objects = 0;
  if (!asi::mem::Read<std::uint32_t>(pools + kObjectPoolAt, &objects) ||
      objects == 0)
    return 0;
  if (!asi::mem::IsReadable(objects, kPointersAt + kMaxObjects * 4)) return 0;
  return objects;
}

bool ReadableText(std::uint32_t at, std::string* out, std::size_t most) {
  if (at < 0x10000) return false;
  char buffer[kMaxTextBytes + 1] = {};
  if (most > kMaxTextBytes) most = kMaxTextBytes;
  const std::size_t got = asi::mem::ReadGuarded(at, buffer, most);
  if (got < 2) return false;
  const std::size_t length = ::strnlen(buffer, got);
  if (length < 1 || length >= got) return false;
  for (std::size_t i = 0; i < length; ++i)
    if (static_cast<unsigned char>(buffer[i]) < 0x09) return false;
  if (out != nullptr) out->assign(buffer, length);
  return true;
}

// Only when the sum has found nothing at all: where in an object does a
// pointer to a readable string actually sit?
void Calibrate(std::uintptr_t object) {
  if (g_calibrated) return;
  g_calibrated = true;
  std::string found;
  for (std::uint32_t at = 0x200; at < 0x1600; at += 4) {
    std::uint32_t pointer = 0;
    if (!asi::mem::Read<std::uint32_t>(object + at, &pointer)) continue;
    std::string text;
    if (!ReadableText(pointer, &text, 64)) continue;
    found += " +0x" + [&] {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "%X", at);
      return std::string(buffer);
    }() + " \"" + text.substr(0, 40) + "\"";
  }
  LOG_WARN("object text: nothing at the offset the structures give (+0x{:X}); "
           "an object holds string pointers at:{}", kTextAt,
           found.empty() ? " none" : found);
}

}  // namespace

std::vector<ObjectText> ObjectTextsNear(const Vec3& at, float radius,
                                        std::size_t max) {
  std::vector<ObjectText> found;
  const std::uintptr_t pool = Pool();
  if (pool == 0) {
    g_note = "the client's object pool is not where this build keeps it";
    return found;
  }
  const float radius_squared = radius * radius;
  int in_use = 0, with_text = 0;
  std::uintptr_t first_object = 0;

  for (int i = 0; i < kMaxObjects; ++i) {
    std::uint32_t used = 0;
    if (!asi::mem::Read<std::uint32_t>(pool + kInUseAt + i * 4, &used) || used == 0)
      continue;
    std::uint32_t object = 0;
    if (!asi::mem::Read<std::uint32_t>(pool + kPointersAt + i * 4, &object) ||
        object == 0)
      continue;
    ++in_use;
    if (first_object == 0) first_object = object;

    Vec3 where{};
    asi::mem::Read<float>(object + kPositionAt + 0, &where.x);
    asi::mem::Read<float>(object + kPositionAt + 4, &where.y);
    asi::mem::Read<float>(object + kPositionAt + 8, &where.z);
    if (!(where.x == where.x) || std::fabs(where.x) > kWorldEdge) continue;
    const float dx = where.x - at.x, dy = where.y - at.y, dz = where.z - at.z;
    const float d2 = dx * dx + dy * dy + dz * dz;

    for (int m = 0; m < kMaterials; ++m) {
      std::int32_t type = 0;
      if (!asi::mem::Read<std::int32_t>(object + kTypeAt + m * 4, &type)) break;
      if (type != kTypeText) continue;
      std::uint32_t text = 0;
      if (!asi::mem::Read<std::uint32_t>(object + kTextAt + m * 4, &text)) continue;
      ObjectText painted;
      if (!ReadableText(text, &painted.text, kMaxTextBytes)) continue;
      ++with_text;
      if (d2 > radius_squared || found.size() >= max) continue;

      painted.object_id = i;
      painted.material = m;
      painted.at = where;
      painted.away_m = std::sqrt(d2);
      painted.text = ToUtf8(painted.text);
      std::int32_t model = 0;
      asi::mem::Read<std::int32_t>(object + kModelAt, &model);
      painted.model = model;
      std::string font;
      const std::uint32_t settings = object + kSettingsAt +
                                     static_cast<std::uint32_t>(m) * kSettingsBytes;
      char raw[66] = {};
      if (asi::mem::ReadGuarded(settings + kFontAt, raw, 65) == 65)
        painted.font = ToUtf8(std::string(raw, ::strnlen(raw, 65)));
      found.push_back(std::move(painted));
    }
  }

  if (with_text == 0 && first_object != 0) Calibrate(first_object);
  g_note = std::to_string(in_use) + " of the server's objects are here, " +
           std::to_string(with_text) + " of them with text painted on";
  std::sort(found.begin(), found.end(),
            [](const ObjectText& a, const ObjectText& b) {
              return a.away_m < b.away_m;
            });
  return found;
}

std::string ObjectTextsNote() { return g_note; }

}  // namespace gtabot::samp
