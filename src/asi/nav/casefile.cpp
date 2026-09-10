#include "nav/casefile.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace gtabot::nav {
namespace {

constexpr char kMagic[8] = {'G', 'T', 'A', 'B', 'O', 'T', 'F', '2'};
constexpr float kCentimetre = 0.01f;
constexpr std::int16_t kNoGround = -32768;

template <typename T>
bool Put(std::FILE* f, const T& value) {
  return std::fwrite(&value, sizeof(T), 1, f) == 1;
}

template <typename T>
bool Get(std::FILE* f, T* value) {
  return std::fread(value, sizeof(T), 1, f) == 1;
}

bool PutText(std::FILE* f, const std::string& text) {
  const std::uint32_t length = static_cast<std::uint32_t>(text.size());
  if (!Put(f, length)) return false;
  if (length == 0) return true;
  return std::fwrite(text.data(), 1, length, f) == length;
}

bool GetText(std::FILE* f, std::string* text) {
  std::uint32_t length = 0;
  if (!Get(f, &length) || length > (1u << 20)) return false;
  text->assign(length, '\0');
  if (length == 0) return true;
  return std::fread(&(*text)[0], 1, length, f) == length;
}

}  // namespace

void CaseShot::Take(const Grid& g, float reference) {
  W = g.W;
  H = g.H;
  cell = g.cell;
  x0 = g.x0;
  y0 = g.y0;
  ref_z = reference;
  const std::size_t cells = static_cast<std::size_t>(W) * H;
  if (W <= 0 || H <= 0 || g.ground.size() < cells || g.known.size() < cells ||
      g.blocked.size() < cells) {
    W = H = 0;
    flags.clear();
    floor.clear();
    return;
  }
  flags.resize(cells);
  floor.resize(cells);
  for (std::size_t at = 0; at < cells; ++at) {
    flags[at] = static_cast<std::uint8_t>((g.known[at] & 0x3) |
                                          (g.blocked[at] ? 0x4 : 0));
    if (g.known[at] != 1) {
      floor[at] = kNoGround;
      continue;
    }
    const float centimetres = (g.ground[at] - ref_z) / kCentimetre;
    floor[at] = centimetres > 32000.0f    ? 32000
                : centimetres < -32000.0f ? -32000
                                          : static_cast<std::int16_t>(centimetres);
  }
}

namespace {

bool WriteCase(const CaseShot& shot, const FieldCase& one,
               const std::string& path) {
  if (shot.empty()) return false;
  const std::size_t cells = static_cast<std::size_t>(shot.W) * shot.H;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  bool ok = std::fwrite(kMagic, 1, sizeof(kMagic), f) == sizeof(kMagic);
  ok = ok && Put(f, shot.W) && Put(f, shot.H) && Put(f, shot.cell) &&
       Put(f, shot.x0) && Put(f, shot.y0);
  ok = ok && Put(f, one.from.x) && Put(f, one.from.y) && Put(f, one.from.z);
  ok = ok && Put(f, one.to.x) && Put(f, one.to.y) && Put(f, one.to.z);
  ok = ok && Put(f, shot.ref_z) && Put(f, one.stride);
  ok = ok && PutText(f, one.note) && PutText(f, one.why);
  const std::uint32_t points = static_cast<std::uint32_t>(one.route.size());
  ok = ok && Put(f, points);
  for (std::uint32_t i = 0; ok && i < points; ++i)
    ok = Put(f, one.route[i].x) && Put(f, one.route[i].y) &&
         Put(f, one.route[i].z);
  ok = ok && std::fwrite(shot.flags.data(), 1, cells, f) == cells;
  ok = ok && std::fwrite(shot.floor.data(), sizeof(std::int16_t), cells, f) == cells;
  std::fclose(f);
  if (!ok) std::remove(path.c_str());
  return ok;
}

}  // namespace

bool SaveShot(const CaseShot& shot, const FieldCase& context,
              const std::string& path) {
  return WriteCase(shot, context, path);
}

bool SaveCase(const FieldCase& one, const std::string& path) {
  CaseShot shot;
  shot.Take(one.grid, one.ref_z);
  return WriteCase(shot, one, path);
}

bool LoadCase(const std::string& path, FieldCase* out) {
  if (out == nullptr) return false;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  char magic[8] = {0};
  bool ok = std::fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
            std::memcmp(magic, kMagic, sizeof(kMagic)) == 0;
  Grid& g = out->grid;
  int w = 0, h = 0;
  ok = ok && Get(f, &w) && Get(f, &h) && Get(f, &g.cell) && Get(f, &g.x0) &&
       Get(f, &g.y0);
  if (ok && (w <= 0 || h <= 0 || static_cast<long long>(w) * h > 40000000LL))
    ok = false;
  ok = ok && Get(f, &out->from.x) && Get(f, &out->from.y) && Get(f, &out->from.z);
  ok = ok && Get(f, &out->to.x) && Get(f, &out->to.y) && Get(f, &out->to.z);
  ok = ok && Get(f, &out->ref_z) && Get(f, &out->stride);
  ok = ok && GetText(f, &out->note) && GetText(f, &out->why);
  std::uint32_t points = 0;
  ok = ok && Get(f, &points) && points < 100000;
  out->route.clear();
  for (std::uint32_t i = 0; ok && i < points; ++i) {
    Vec3 p;
    ok = Get(f, &p.x) && Get(f, &p.y) && Get(f, &p.z);
    if (ok) out->route.push_back(p);
  }
  if (ok) {
    g.Resize(w, h);
    const std::size_t cells = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> flags(cells);
    std::vector<std::int16_t> floor(cells);
    ok = std::fread(flags.data(), 1, cells, f) == cells;
    ok = ok && std::fread(floor.data(), sizeof(std::int16_t), cells, f) == cells;
    if (ok)
      for (std::size_t at = 0; at < cells; ++at) {
        g.known[at] = static_cast<std::uint8_t>(flags[at] & 0x3);
        g.blocked[at] = (flags[at] & 0x4) != 0 ? 1 : 0;
        g.ground[at] = floor[at] == kNoGround
                           ? 0.0f
                           : out->ref_z + floor[at] * kCentimetre;
        g.clear[at] = 0;
      }
  }
  std::fclose(f);
  return ok;
}

}  // namespace gtabot::nav
