#include "nav/local.hpp"

#include <cmath>

namespace gtabot::nav {

float LocalPicture::FreeAlong(const Vec3& a, const Vec3& b, float slack,
                              float* low_at) const {
  if (low_at != nullptr) *low_at = -1.0f;
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (!ok || high.W == 0 || length < 1e-3f) return length;
  // Half a cell a sample: nothing a cell wide falls between two of them.
  const float step = high.cell * 0.5f;
  const int samples = static_cast<int>(std::ceil(length / step));
  for (int i = 0; i <= samples; ++i) {
    const float d = std::min(length, i * step);
    if (d < slack) continue;
    const Vec3 p{a.x + dx / length * d, a.y + dy / length * d, 0};
    int ix = 0, iy = 0;
    if (!high.cell_of(p, &ix, &iy)) return length;
    const int at = high.index(ix, iy);
    if (!high.passable(at)) return d;
    if (low_at != nullptr && *low_at < 0 && low[static_cast<std::size_t>(at)])
      *low_at = d;
  }
  return length;
}

float LocalPicture::Clearance(const Vec3& p) const {
  if (!ok || high.W == 0) return radius;
  int ix = 0, iy = 0;
  if (!high.cell_of(p, &ix, &iy)) return radius;
  // The chamfer counts three a cell along a side; a shut cell is nought.
  return high.clear[high.index(ix, iy)] / 3.0f * high.cell;
}

bool LocalPicture::Shut(const Vec3& p) const {
  if (!ok || high.W == 0) return false;
  int ix = 0, iy = 0;
  if (!high.cell_of(p, &ix, &iy)) return false;
  return !high.passable(high.index(ix, iy));
}

}  // namespace gtabot::nav
