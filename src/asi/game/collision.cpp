#include "game/collision.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "game/exe.hpp"
#include "game/world_query.hpp"
#include "log.hpp"
#include "state/memory.hpp"

namespace gtabot::game::col {
namespace {

// ---- where the game keeps it (gta_sa.exe 1.0 US) ----
//
// The sector array at 0xB7D0B8 is supposed to hold a building list and a
// dummy list per sector, and on this client both read as empty while the
// repeat sectors beside it read correctly. Rather than guess at that array,
// the static world is taken from the pools it is allocated out of - every
// building and every dummy the game has - and sorted into a grid of our
// own. Moving things (objects, vehicles) still come from the repeat
// sectors, which the log proved right.
constexpr std::uint32_t kBuildingPool  = 0xB74498;   // CPool<CBuilding>*
constexpr std::uint32_t kDummyPool     = 0xB744A0;   // CPool<CDummy>*
constexpr std::uint32_t kRepeatSectors = 0xB992B8;   // CRepeatSector[16][16]
// CStreaming::RequestModelStream reads CModelInfo::ms_modelInfoPtrs through
// this operand; a limit adjuster that moves the table rewrites it here.
constexpr std::uint32_t kModelTableOperand = 0x40CD67;
constexpr std::uint32_t kModelTableExpected = 0xA9B0C8;

// CPool: the objects, the byte map, the size.
constexpr std::uint32_t kPoolObjects = 0x00, kPoolByteMap = 0x04, kPoolSize = 0x08;
constexpr std::uint32_t kEntityStride = 0x38;   // CBuilding and CDummy alike
constexpr std::uint8_t  kSlotEmpty = 0x80;      // tPoolObjectFlags: bEmpty is bit 7
constexpr int kMaxPoolSize = 200000;

constexpr int   kSectorsX = 120, kSectorsY = 120;
constexpr float kSectorMetres = 50.0f;
constexpr int   kRepeat = 16;
constexpr std::uint32_t kRepeatSectorSize = 12;
constexpr std::uint32_t kRepeatVehicles = 0, kRepeatObjects = 8;
constexpr int kMaxListNodes = 8192;

// CEntity.
constexpr std::uint32_t kEntityPosition = 0x04;   // CSimpleTransform, when no matrix
constexpr std::uint32_t kEntityHeading  = 0x10;
constexpr std::uint32_t kEntityMatrix   = 0x14;   // CMatrixLink*
constexpr std::uint32_t kEntityFlags    = 0x1C;   // bit 0: bUsesCollision
constexpr std::uint32_t kEntityModel    = 0x22;   // int16
// RwMatrix rows.
constexpr std::uint32_t kRight = 0x00, kForward = 0x10, kUp = 0x20, kPos = 0x30;
// CBaseModelInfo, CColModel, CCollisionData.
constexpr std::uint32_t kModelColModel  = 0x14;
constexpr std::uint32_t kColBoxMin = 0x00, kColBoxMax = 0x0C;
constexpr std::uint32_t kColData   = 0x2C;
constexpr std::uint32_t kDataNumSpheres = 0, kDataNumBoxes = 2, kDataNumTriangles = 4;
constexpr std::uint32_t kDataFlags = 0x07;        // bit 1: face groups present
constexpr std::uint8_t  kHasFaceGroups = 0x02;
constexpr std::uint32_t kDataSpheres = 0x08, kDataBoxes = 0x0C, kDataVertices = 0x14,
                        kDataTriangles = 0x18;
// Face groups: the game's own acceleration for the triangles. The count sits
// in the four bytes before the triangle array, the groups before that, each
// a bounding box and the range of triangles inside it.
constexpr std::uint32_t kFaceGroupSize = 28;
constexpr std::uint32_t kFaceGroupFirst = 0x18, kFaceGroupLast = 0x1A;
constexpr unsigned kMaxFaceGroups = 4096;
constexpr std::uint32_t kSphereSize = 0x14, kBoxSize = 0x1C, kTriangleSize = 0x08;
constexpr float kVertexScale = 1.0f / 128.0f;
constexpr int   kMaxPrimitives = 20000;
// The box of a boom gate (bar_gatebox01), whose arm is bar_gatebar01, 968.
constexpr std::int16_t kGateBox = 966;
constexpr float kEpsilon = 1e-6f;

// Our own grid: a square of sectors around wherever the questions are being
// asked, rebuilt as they move towards its edge or as it grows stale.
constexpr int   kWindow = 21;                 // sectors on a side, 1050 m
constexpr int   kWindowMargin = 3;            // rebuilt before the edge is reached
constexpr unsigned long long kRebuildEveryMs = 4000;
constexpr float kMaxSpread = 150.0f;          // how far one thing may reach

std::atomic<std::uint32_t> g_model_table{0};
std::atomic<bool> g_checked{false};
std::atomic<bool> g_ready{false};
std::atomic<unsigned long long> g_queries{0}, g_entities{0}, g_primitives{0};

std::vector<std::uintptr_t> g_bucket[kWindow * kWindow];
int  g_window_x0 = 0, g_window_y0 = 0;
bool g_window_built = false;
unsigned long long g_window_ms = 0;
int  g_indexed_buildings = 0, g_indexed_dummies = 0;
int  g_building_slots = 0, g_dummy_slots = 0;
bool g_said_index = false;

struct V {
  float x, y, z;
};

V Sub(V a, V b) { return V{a.x - b.x, a.y - b.y, a.z - b.z}; }
V Cross(V a, V b) {
  return V{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// One query: a segment, and whether the nearest hit is wanted (the ground)
// or any hit will do (a line). Plain data: it lives inside __try.
struct Query {
  V     a, b;
  bool  nearest;
  bool  vehicles;
  // Whether objects count. The game's own ground probe asks buildings and
  // dummies only - a crate, a barrier or whatever the server has built out
  // of objects is not ground to it - while its line of sight asks objects
  // too. Ours does the same, or the roof of a server-built object thirty
  // metres up becomes the ground under a marker.
  bool  objects;
  bool  hit;
  float best_t;
  int   entities;      // with a collision model
  int   boxed;         // whose bounding box the segment crosses
  int   with_data;     // of those, with collision data loaded
  int   primitives;
  // The first few entities, for an explanation.
  int   noted;
  char  notes[6][160];
};

// Reads inside the guarded traversal: plain dereferences, the page fault
// being the only failure and the guard its handler.
inline std::uint32_t U32(std::uintptr_t at) { return *reinterpret_cast<const std::uint32_t*>(at); }
inline std::uint16_t U16(std::uintptr_t at) { return *reinterpret_cast<const std::uint16_t*>(at); }
inline std::int16_t  S16(std::uintptr_t at) { return *reinterpret_cast<const std::int16_t*>(at); }
inline std::uint8_t  U8(std::uintptr_t at)  { return *reinterpret_cast<const std::uint8_t*>(at); }
inline float         F32(std::uintptr_t at) { return *reinterpret_cast<const float*>(at); }
inline V             Vec(std::uintptr_t at) { return V{F32(at), F32(at + 4), F32(at + 8)}; }

bool Plausible(std::uint32_t pointer) {
  return pointer >= 0x10000 && pointer < 0x7FFF0000 && (pointer & 3) == 0;
}

int SectorX(float x) { return static_cast<int>(std::floor(x / kSectorMetres + kSectorsX / 2)); }
int SectorY(float y) { return static_cast<int>(std::floor(y / kSectorMetres + kSectorsY / 2)); }

void Consider(Query& q, float t) {
  if (t < 0.0f || t > 1.0f) return;
  if (!q.hit || t < q.best_t) q.best_t = t;
  q.hit = true;
}

// Segment a + t d against an axis-aligned box: the entry parameter, or
// false. Starting inside counts as entering at zero.
bool SlabEntry(V a, V d, V lo, V hi, float* entry) {
  float t0 = 0.0f, t1 = 1.0f;
  const float* av = &a.x;
  const float* dv = &d.x;
  const float* lv = &lo.x;
  const float* hv = &hi.x;
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(dv[i]) < kEpsilon) {
      if (av[i] < lv[i] || av[i] > hv[i]) return false;
      continue;
    }
    float ta = (lv[i] - av[i]) / dv[i];
    float tb = (hv[i] - av[i]) / dv[i];
    if (ta > tb) { const float s = ta; ta = tb; tb = s; }
    if (ta > t0) t0 = ta;
    if (tb < t1) t1 = tb;
    if (t0 > t1) return false;
  }
  *entry = t0;
  return true;
}

bool SphereEntry(V a, V d, V c, float r, float* entry) {
  const V m = Sub(a, c);
  const float A = Dot(d, d);
  if (A < kEpsilon) return false;
  const float B = 2.0f * Dot(m, d);
  const float C = Dot(m, m) - r * r;
  const float disc = B * B - 4.0f * A * C;
  if (disc < 0.0f) return false;
  const float root = std::sqrt(disc);
  float t = (-B - root) / (2.0f * A);
  const float t_exit = (-B + root) / (2.0f * A);
  if (t_exit < 0.0f || t > 1.0f) return false;
  if (t < 0.0f) t = 0.0f;   // starts inside
  *entry = t;
  return true;
}

// Moller-Trumbore, both faces.
bool TriangleEntry(V a, V d, V v0, V v1, V v2, float* entry) {
  const V e1 = Sub(v1, v0);
  const V e2 = Sub(v2, v0);
  const V p = Cross(d, e2);
  const float det = Dot(e1, p);
  if (std::fabs(det) < kEpsilon) return false;
  const float inv = 1.0f / det;
  const V s = Sub(a, v0);
  const float u = Dot(s, p) * inv;
  if (u < -kEpsilon || u > 1.0f + kEpsilon) return false;
  const V qv = Cross(s, e1);
  const float v = Dot(d, qv) * inv;
  if (v < -kEpsilon || u + v > 1.0f + kEpsilon) return false;
  const float t = Dot(e2, qv) * inv;
  if (t < 0.0f || t > 1.0f) return false;
  *entry = t;
  return true;
}

// The entity's collision against the query, in the entity's own space.
void TestEntity(Query& q, std::uintptr_t entity) {
  if (!Plausible(static_cast<std::uint32_t>(entity))) return;
  const std::uint32_t flags = U32(entity + kEntityFlags);
  if ((flags & 1) == 0) return;                        // no collision
  const std::int16_t model = S16(entity + kEntityModel);
  if (model < 0) return;
  const std::uint32_t table = g_model_table.load(std::memory_order_relaxed);
  const std::uint32_t info = U32(table + static_cast<std::uint32_t>(model) * 4);
  if (!Plausible(info)) return;
  const std::uint32_t colmodel = U32(info + kModelColModel);
  if (!Plausible(colmodel)) return;
  ++q.entities;

  // Into the entity's space.
  V right, forward, up, pos;
  const std::uint32_t matrix = U32(entity + kEntityMatrix);
  if (Plausible(matrix)) {
    right   = Vec(matrix + kRight);
    forward = Vec(matrix + kForward);
    up      = Vec(matrix + kUp);
    pos     = Vec(matrix + kPos);
  } else {
    pos = Vec(entity + kEntityPosition);
    const float h = F32(entity + kEntityHeading);
    right   = V{std::cos(h), std::sin(h), 0.0f};
    forward = V{-std::sin(h), std::cos(h), 0.0f};
    up      = V{0.0f, 0.0f, 1.0f};
  }
  const V ra = Sub(q.a, pos);
  const V rb = Sub(q.b, pos);
  const V a{Dot(ra, right), Dot(ra, forward), Dot(ra, up)};
  const V b{Dot(rb, right), Dot(rb, forward), Dot(rb, up)};
  const V d = Sub(b, a);

  // The bounding box first; most entities end here.
  float entry = 0;
  const V lo = Vec(colmodel + kColBoxMin);
  const V hi = Vec(colmodel + kColBoxMax);
  const std::uint32_t data = U32(colmodel + kColData);
  const bool boxed = SlabEntry(a, d, V{lo.x - 0.05f, lo.y - 0.05f, lo.z - 0.05f},
                               V{hi.x + 0.05f, hi.y + 0.05f, hi.z + 0.05f}, &entry);
  if (q.noted < 6) {
    std::snprintf(q.notes[q.noted], sizeof(q.notes[q.noted]),
                  "entity 0x%08X model %d %s pos (%.1f,%.1f,%.1f) local start (%.1f,%.1f,%.1f) "
                  "box (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) data 0x%08X%s",
                  static_cast<unsigned>(entity), model, Plausible(matrix) ? "matrix" : "placed",
                  pos.x, pos.y, pos.z, a.x, a.y, a.z, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z,
                  data, boxed ? " boxed" : "");
    ++q.noted;
  }
  if (!boxed) return;
  ++q.boxed;

  if (!Plausible(data)) return;
  ++q.with_data;
  const unsigned spheres   = U16(data + kDataNumSpheres);
  const unsigned boxes     = U16(data + kDataNumBoxes);
  const unsigned triangles = U16(data + kDataNumTriangles);
  if (spheres > kMaxPrimitives || boxes > kMaxPrimitives || triangles > kMaxPrimitives) return;
  q.primitives += static_cast<int>(spheres + boxes + triangles);

  const std::uint32_t sphere_array = U32(data + kDataSpheres);
  for (unsigned i = 0; i < spheres && Plausible(sphere_array); ++i) {
    const std::uintptr_t s = sphere_array + i * kSphereSize;
    if (SphereEntry(a, d, Vec(s), F32(s + 0xC), &entry)) {
      Consider(q, entry);
      if (!q.nearest) return;
    }
  }
  const std::uint32_t box_array = U32(data + kDataBoxes);
  for (unsigned i = 0; i < boxes && Plausible(box_array); ++i) {
    const std::uintptr_t bx = box_array + i * kBoxSize;
    if (SlabEntry(a, d, Vec(bx), Vec(bx + 0xC), &entry)) {
      Consider(q, entry);
      if (!q.nearest) return;
    }
  }
  const std::uint32_t tri_array = U32(data + kDataTriangles);
  const std::uint32_t vertices  = U32(data + kDataVertices);
  if (!Plausible(tri_array) || !Plausible(vertices)) return;

  // The segment's own box, to throw triangles out by.
  const V seg_lo{a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
  const V seg_hi{a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};

  // Whole groups of triangles at a time, where the model has them: one box
  // per group against the segment's, and the ones that miss are skipped
  // without a vertex being read. A building is thousands of triangles and
  // a handful of groups.
  unsigned groups = 0;
  std::uint32_t group_array = 0;
  if ((U8(data + kDataFlags) & kHasFaceGroups) != 0) {
    groups = U32(tri_array - 4);
    if (groups == 0 || groups > kMaxFaceGroups) {
      groups = 0;
    } else {
      group_array = tri_array - 4 - groups * kFaceGroupSize;
      if (!Plausible(group_array)) groups = 0;
    }
  }
  if (groups != 0) {
    for (unsigned g = 0; g < groups; ++g) {
      const std::uintptr_t fg = group_array + g * kFaceGroupSize;
      const V lo2 = Vec(fg);
      const V hi2 = Vec(fg + 0x0C);
      if (seg_hi.x < lo2.x || seg_lo.x > hi2.x || seg_hi.y < lo2.y || seg_lo.y > hi2.y ||
          seg_hi.z < lo2.z || seg_lo.z > hi2.z)
        continue;
      const unsigned first = U16(fg + kFaceGroupFirst);
      const unsigned last  = U16(fg + kFaceGroupLast);
      if (first > last || last >= triangles) continue;
      for (unsigned i = first; i <= last; ++i) {
        const std::uintptr_t t = tri_array + i * kTriangleSize;
        const std::uintptr_t va = vertices + U16(t) * 6u;
        const std::uintptr_t vb = vertices + U16(t + 2) * 6u;
        const std::uintptr_t vc = vertices + U16(t + 4) * 6u;
        const V v0{S16(va) * kVertexScale, S16(va + 2) * kVertexScale, S16(va + 4) * kVertexScale};
        const V v1{S16(vb) * kVertexScale, S16(vb + 2) * kVertexScale, S16(vb + 4) * kVertexScale};
        const V v2{S16(vc) * kVertexScale, S16(vc + 2) * kVertexScale, S16(vc + 4) * kVertexScale};
        if (TriangleEntry(a, d, v0, v1, v2, &entry)) {
          Consider(q, entry);
          if (!q.nearest) return;
        }
      }
    }
    return;
  }

  for (unsigned i = 0; i < triangles; ++i) {
    const std::uintptr_t t = tri_array + i * kTriangleSize;
    const std::uintptr_t va = vertices + U16(t) * 6u;
    const std::uintptr_t vb = vertices + U16(t + 2) * 6u;
    const std::uintptr_t vc = vertices + U16(t + 4) * 6u;
    const V v0{S16(va) * kVertexScale, S16(va + 2) * kVertexScale, S16(va + 4) * kVertexScale};
    const V v1{S16(vb) * kVertexScale, S16(vb + 2) * kVertexScale, S16(vb + 4) * kVertexScale};
    const V v2{S16(vc) * kVertexScale, S16(vc + 2) * kVertexScale, S16(vc + 4) * kVertexScale};
    // Nowhere near the segment: no need for the arithmetic.
    const float tri_lo_x = v0.x < v1.x ? (v0.x < v2.x ? v0.x : v2.x) : (v1.x < v2.x ? v1.x : v2.x);
    const float tri_hi_x = v0.x > v1.x ? (v0.x > v2.x ? v0.x : v2.x) : (v1.x > v2.x ? v1.x : v2.x);
    if (seg_hi.x < tri_lo_x || seg_lo.x > tri_hi_x) continue;
    const float tri_lo_y = v0.y < v1.y ? (v0.y < v2.y ? v0.y : v2.y) : (v1.y < v2.y ? v1.y : v2.y);
    const float tri_hi_y = v0.y > v1.y ? (v0.y > v2.y ? v0.y : v2.y) : (v1.y > v2.y ? v1.y : v2.y);
    if (seg_hi.y < tri_lo_y || seg_lo.y > tri_hi_y) continue;
    if (TriangleEntry(a, d, v0, v1, v2, &entry)) {
      Consider(q, entry);
      if (!q.nearest) return;
    }
  }
}

// ---- painting the world onto a grid ----

struct Paint {
  Footprint* out = nullptr;
  const std::vector<Leaf>* skip = nullptr;   // leaves left unpainted, by position
  float z_lo = 0, z_hi = 0;     // the band, in the world, over the default floor
  float inflate = 0;
  float x1 = 0, y1 = 0;         // the far corner of the square
  long  budget = 0;             // cell tests left
  // The band as offsets, and the floors to judge it against per cell. With
  // no floors every cell uses the default, which is the old behaviour.
  const Floors* floors = nullptr;
  bool  vehicles = false;
  float band_lo = 0, band_hi = 0, floor_default = 0;
  float floor_min = 0, floor_max = 0;   // over the square, for the early out
};

struct P2 { float x, y; };

// The floor under a point: the reading there when there is one, the
// square's default otherwise.
float FloorAt(const Paint& p, float px, float py) {
  const Floors* f = p.floors;
  if (f == nullptr || f->z == nullptr) return p.floor_default;
  const int ix = static_cast<int>(std::floor((px - f->x0) / f->cell));
  const int iy = static_cast<int>(std::floor((py - f->y0) / f->cell));
  if (ix < 0 || iy < 0 || ix >= f->w || iy >= f->h) return p.floor_default;
  const std::size_t at = static_cast<std::size_t>(iy) * f->w + ix;
  if (f->known != nullptr && f->known[at] != 1) return p.floor_default;
  return f->z[at];
}

// A surface's height at a point, from three of its corners. Only asked of
// surfaces that are not near vertical.
float HeightOn(const V& a, const V& b, const V& c, float px, float py) {
  const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
  const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
  const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
  if (std::fabs(nz) < 1e-6f) return a.z;
  return a.z - (nx * (px - a.x) + ny * (py - a.y)) / nz;
}

// The cells within reach of a polygon: inside it, or nearer to one of its
// edges than the inflation. Works for a triangle, a rotated box's hull, and
// a wall seen edge-on - a two-point "polygon" - which is the one that
// matters most.
// `surface`, when given, is the three corners of a floor-like triangle: at
// each cell the triangle's own height there decides, so a ramp that is the
// ground under a cell is not painted at that cell, while a wall - judged by
// its span - is.
void PaintPolygon(Paint& p, const P2* poly, int n, float lo_z, float hi_z,
                  const V* surface = nullptr) {
  if (n < 2) return;
  if (hi_z < p.floor_min + p.band_lo || lo_z > p.floor_max + p.band_hi) return;
  Footprint& f = *p.out;
  float min_x = poly[0].x, max_x = poly[0].x, min_y = poly[0].y, max_y = poly[0].y;
  for (int i = 1; i < n; ++i) {
    min_x = poly[i].x < min_x ? poly[i].x : min_x;
    max_x = poly[i].x > max_x ? poly[i].x : max_x;
    min_y = poly[i].y < min_y ? poly[i].y : min_y;
    max_y = poly[i].y > max_y ? poly[i].y : max_y;
  }
  if (max_x < f.x0 - p.inflate || min_x > p.x1 + p.inflate ||
      max_y < f.y0 - p.inflate || min_y > p.y1 + p.inflate)
    return;
  int cx0 = static_cast<int>(std::floor((min_x - p.inflate - f.x0) / f.cell));
  int cx1 = static_cast<int>(std::floor((max_x + p.inflate - f.x0) / f.cell));
  int cy0 = static_cast<int>(std::floor((min_y - p.inflate - f.y0) / f.cell));
  int cy1 = static_cast<int>(std::floor((max_y + p.inflate - f.y0) / f.cell));
  cx0 = cx0 < 0 ? 0 : cx0;
  cy0 = cy0 < 0 ? 0 : cy0;
  cx1 = cx1 >= f.side ? f.side - 1 : cx1;
  cy1 = cy1 >= f.side ? f.side - 1 : cy1;
  const float r2 = p.inflate * p.inflate;
  for (int cy = cy0; cy <= cy1; ++cy) {
    for (int cx = cx0; cx <= cx1; ++cx) {
      if (--p.budget < 0) { f.starved = true; return; }
      std::uint8_t& cell = f.blocked[static_cast<std::size_t>(cy) * f.side + cx];
      if (cell) continue;
      const float px = f.x0 + (cx + 0.5f) * f.cell;
      const float py = f.y0 + (cy + 0.5f) * f.cell;
      // This cell's own band.
      const float floor = FloorAt(p, px, py);
      const float blo = floor + p.band_lo, bhi = floor + p.band_hi;
      if (surface != nullptr) {
        float zc = HeightOn(surface[0], surface[1], surface[2], px, py);
        zc = zc < lo_z ? lo_z : (zc > hi_z ? hi_z : zc);
        if (zc < blo || zc > bhi) continue;      // the ground itself, or overhead
      } else if (hi_z < blo || lo_z > bhi) {
        continue;
      }
      bool inside = false;
      if (n >= 3) {
        for (int i = 0, j = n - 1; i < n; j = i++) {
          const bool cross = (poly[i].y > py) != (poly[j].y > py);
          if (!cross) continue;
          const float x = poly[j].x + (py - poly[j].y) * (poly[i].x - poly[j].x) /
                                          (poly[i].y - poly[j].y);
          if (px < x) inside = !inside;
        }
      }
      if (inside) {
        cell = 1;
        ++f.painted;
        continue;
      }
      for (int i = 0, j = n - 1; i < n; j = i++) {
        const float ex = poly[i].x - poly[j].x, ey = poly[i].y - poly[j].y;
        const float len2 = ex * ex + ey * ey;
        float t = len2 > 1e-8f ? ((px - poly[j].x) * ex + (py - poly[j].y) * ey) / len2 : 0.0f;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        const float dx = px - (poly[j].x + ex * t), dy = py - (poly[j].y + ey * t);
        if (dx * dx + dy * dy <= r2) {
          cell = 1;
          ++f.painted;
          break;
        }
      }
    }
  }
}

void PaintCircle(Paint& p, float cx, float cy, float r, float lo_z, float hi_z) {
  if (hi_z < p.floor_min + p.band_lo || lo_z > p.floor_max + p.band_hi) return;
  Footprint& f = *p.out;
  const float reach = r + p.inflate;
  int cx0 = static_cast<int>(std::floor((cx - reach - f.x0) / f.cell));
  int cx1 = static_cast<int>(std::floor((cx + reach - f.x0) / f.cell));
  int cy0 = static_cast<int>(std::floor((cy - reach - f.y0) / f.cell));
  int cy1 = static_cast<int>(std::floor((cy + reach - f.y0) / f.cell));
  cx0 = cx0 < 0 ? 0 : cx0;
  cy0 = cy0 < 0 ? 0 : cy0;
  cx1 = cx1 >= f.side ? f.side - 1 : cx1;
  cy1 = cy1 >= f.side ? f.side - 1 : cy1;
  for (int y = cy0; y <= cy1; ++y)
    for (int x = cx0; x <= cx1; ++x) {
      if (--p.budget < 0) { f.starved = true; return; }
      std::uint8_t& cell = f.blocked[static_cast<std::size_t>(y) * f.side + x];
      if (cell) continue;
      const float px = f.x0 + (x + 0.5f) * f.cell, py = f.y0 + (y + 0.5f) * f.cell;
      const float floor = FloorAt(p, px, py);
      if (hi_z < floor + p.band_lo || lo_z > floor + p.band_hi) continue;
      const float dx = px - cx, dy = py - cy;
      if (dx * dx + dy * dy <= reach * reach) {
        cell = 1;
        ++f.painted;
      }
    }
}

// The convex hull of a few points - the eight corners of a box - by the
// monotone chain. Returns how many points of `out` are the hull.
int Hull(P2* pts, int n, P2* out) {
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && (pts[j].x < pts[j - 1].x ||
                              (pts[j].x == pts[j - 1].x && pts[j].y < pts[j - 1].y)); --j) {
      const P2 t = pts[j];
      pts[j] = pts[j - 1];
      pts[j - 1] = t;
    }
  int k = 0;
  const auto cross = [](P2 o, P2 a, P2 b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  };
  for (int i = 0; i < n; ++i) {
    while (k >= 2 && cross(out[k - 2], out[k - 1], pts[i]) <= 0) --k;
    out[k++] = pts[i];
  }
  for (int i = n - 2, t = k + 1; i >= 0; --i) {
    while (k >= t && cross(out[k - 2], out[k - 1], pts[i]) <= 0) --k;
    out[k++] = pts[i];
  }
  return k - 1;
}

void PaintEntity(Paint& p, std::uintptr_t entity) {
  if (!Plausible(static_cast<std::uint32_t>(entity))) return;
  const std::uint32_t flags = U32(entity + kEntityFlags);
  if ((flags & 1) == 0) return;
  const std::int16_t model = S16(entity + kEntityModel);
  if (model < 0) return;
  const std::uint32_t table = g_model_table.load(std::memory_order_relaxed);
  const std::uint32_t info = U32(table + static_cast<std::uint32_t>(model) * 4);
  if (!Plausible(info)) return;
  const std::uint32_t colmodel = U32(info + kModelColModel);
  if (!Plausible(colmodel)) return;

  V right, forward, up, pos;
  const std::uint32_t matrix = U32(entity + kEntityMatrix);
  if (Plausible(matrix)) {
    right = Vec(matrix + kRight);
    forward = Vec(matrix + kForward);
    up = Vec(matrix + kUp);
    pos = Vec(matrix + kPos);
  } else {
    pos = Vec(entity + kEntityPosition);
    const float h = F32(entity + kEntityHeading);
    right = V{std::cos(h), std::sin(h), 0.0f};
    forward = V{-std::sin(h), std::cos(h), 0.0f};
    up = V{0.0f, 0.0f, 1.0f};
  }
  // This very leaf, by where it stands.
  if (p.skip != nullptr)
    for (const Leaf& leaf : *p.skip) {
      const float dx = leaf.x - pos.x, dy = leaf.y - pos.y, dz = leaf.z - pos.z;
      // A metre and a half. The leaf's own origin is not always where the
      // server says the door is - a model's origin sits where the modeller
      // put it, and a server moves a door about its hinge - while the glass
      // panels of the same model that make up a wall stand metres away.
      if (dx * dx + dy * dy + dz * dz < 2.25f) return;
    }

  const auto world = [&](V l) {
    return V{pos.x + right.x * l.x + forward.x * l.y + up.x * l.z,
             pos.y + right.y * l.x + forward.y * l.y + up.y * l.z,
             pos.z + right.z * l.x + forward.z * l.y + up.z * l.z};
  };
  const auto corners = [&](V blo, V bhi, P2* q, float* z0, float* z1) {
    *z0 = 1e9f;
    *z1 = -1e9f;
    int i = 0;
    for (int a = 0; a < 2; ++a)
      for (int b = 0; b < 2; ++b)
        for (int d = 0; d < 2; ++d) {
          const V c = world(V{a ? bhi.x : blo.x, b ? bhi.y : blo.y, d ? bhi.z : blo.z});
          q[i++] = P2{c.x, c.y};
          *z0 = c.z < *z0 ? c.z : *z0;
          *z1 = c.z > *z1 ? c.z : *z1;
        }
  };

  // The bounding box first: nowhere near the square or the band, and none
  // of the primitives are read.
  const V lo = Vec(colmodel + kColBoxMin);
  const V hi = Vec(colmodel + kColBoxMax);
  P2 q[8];
  P2 h[10];
  float z0 = 0, z1 = 0;
  corners(lo, hi, q, &z0, &z1);
  if (z1 < p.floor_min + p.band_lo || z0 > p.floor_max + p.band_hi) return;
  {
    float min_x = q[0].x, max_x = q[0].x, min_y = q[0].y, max_y = q[0].y;
    for (int k = 1; k < 8; ++k) {
      min_x = q[k].x < min_x ? q[k].x : min_x;
      max_x = q[k].x > max_x ? q[k].x : max_x;
      min_y = q[k].y < min_y ? q[k].y : min_y;
      max_y = q[k].y > max_y ? q[k].y : max_y;
    }
    const Footprint& f = *p.out;
    if (max_x < f.x0 - p.inflate || min_x > p.x1 + p.inflate ||
        max_y < f.y0 - p.inflate || min_y > p.y1 + p.inflate)
      return;
  }
  ++p.out->entities;

  const std::uint32_t data = U32(colmodel + kColData);
  if (!Plausible(data)) {
    // No primitives: the bounding box is all there is, and it is solid.
    PaintPolygon(p, h, Hull(q, 8, h), z0, z1);
    return;
  }
  // A boom gate is two things: the box with its two posts, and the arm,
  // which the server swings up for a car that sounds its horn and drops
  // again a few seconds later. The arm is painted as it stands, so a plan
  // made while it was up walked him into it once it was down - and a
  // character cannot sound a horn. The box's own bounding box runs from the
  // pivot post to the rest post, which is exactly the arm when it is down,
  // so the box is painted whole: the gate is shut to someone on foot
  // whatever the arm is doing this moment, and the way round is the way.
  if (model == kGateBox) PaintPolygon(p, h, Hull(q, 8, h), z0, z1);
  const unsigned spheres   = U16(data + kDataNumSpheres);
  const unsigned boxes     = U16(data + kDataNumBoxes);
  const unsigned triangles = U16(data + kDataNumTriangles);
  if (spheres > kMaxPrimitives || boxes > kMaxPrimitives || triangles > kMaxPrimitives) return;
  p.out->primitives += static_cast<int>(spheres + boxes + triangles);

  const std::uint32_t sphere_array = U32(data + kDataSpheres);
  for (unsigned i = 0; i < spheres && Plausible(sphere_array); ++i) {
    const std::uintptr_t s = sphere_array + i * kSphereSize;
    const V c = world(Vec(s));
    const float r = F32(s + 0xC);
    PaintCircle(p, c.x, c.y, r, c.z - r, c.z + r);
  }
  const std::uint32_t box_array = U32(data + kDataBoxes);
  for (unsigned i = 0; i < boxes && Plausible(box_array); ++i) {
    const std::uintptr_t bx = box_array + i * kBoxSize;
    corners(Vec(bx), Vec(bx + 0xC), q, &z0, &z1);
    PaintPolygon(p, h, Hull(q, 8, h), z0, z1);
  }
  const std::uint32_t tri_array = U32(data + kDataTriangles);
  const std::uint32_t vertices  = U32(data + kDataVertices);
  if (!Plausible(tri_array) || !Plausible(vertices)) return;
  for (unsigned i = 0; i < triangles; ++i) {
    if (p.budget < 0) { p.out->starved = true; return; }
    const std::uintptr_t t = tri_array + i * kTriangleSize;
    const std::uintptr_t va = vertices + U16(t) * 6u;
    const std::uintptr_t vb = vertices + U16(t + 2) * 6u;
    const std::uintptr_t vc = vertices + U16(t + 4) * 6u;
    const V v0 = world(V{S16(va) * kVertexScale, S16(va + 2) * kVertexScale, S16(va + 4) * kVertexScale});
    const V v1 = world(V{S16(vb) * kVertexScale, S16(vb + 2) * kVertexScale, S16(vb + 4) * kVertexScale});
    const V v2 = world(V{S16(vc) * kVertexScale, S16(vc + 2) * kVertexScale, S16(vc + 4) * kVertexScale});
    const P2 tri[3] = {{v0.x, v0.y}, {v1.x, v1.y}, {v2.x, v2.y}};
    const float t0 = v0.z < v1.z ? (v0.z < v2.z ? v0.z : v2.z) : (v1.z < v2.z ? v1.z : v2.z);
    const float t1 = v0.z > v1.z ? (v0.z > v2.z ? v0.z : v2.z) : (v1.z > v2.z ? v1.z : v2.z);
    // A floor-like triangle - one that is not near vertical - is judged by
    // its own height at each cell, so a ramp is the ground where it is the
    // ground; a wall is judged by its span. Only with floors to judge by.
    const V* surface = nullptr;
    V corner[3] = {v0, v1, v2};
    if (p.floors != nullptr) {
      const float ux = v1.x - v0.x, uy = v1.y - v0.y, uz = v1.z - v0.z;
      const float vx = v2.x - v0.x, vy = v2.y - v0.y, vz = v2.z - v0.z;
      const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
      const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (len > 1e-6f && std::fabs(nz) / len >= 0.5f) surface = corner;
    }
    PaintPolygon(p, tri, 3, t0, t1, surface);
  }
}

void PaintList(Paint& p, std::uintptr_t head) {
  std::uint32_t node = U32(head);
  for (int n = 0; n < kMaxListNodes && Plausible(node); ++n) {
    PaintEntity(p, U32(node));
    node = U32(node + 4);
  }
}

// One sector's worth: either the things that move - the server's objects
// and, when asked, the vehicles, from the repeat sectors - or the static
// buildings and dummies indexed for it. The two are painted in separate
// passes, moving things first, because the cell budget can run out on a
// street of big meshes, and when it does the last things asked for are the
// ones left out. The server's objects are the few that matter most - a
// gate, a barrier, a wall it built - and they used to be asked for last.
bool PaintSector(Paint* p, int sx, int sy, const std::uintptr_t* bucket, int count,
                 bool moving) {
  __try {
    if (moving) {
      const std::uintptr_t repeat =
          At(kRepeatSectors) +
          ((sy & (kRepeat - 1)) * kRepeat + (sx & (kRepeat - 1))) * kRepeatSectorSize;
      PaintList(*p, repeat + kRepeatObjects);
      if (p->vehicles) PaintList(*p, repeat + kRepeatVehicles);
    } else {
      for (int i = 0; i < count; ++i) PaintEntity(*p, bucket[i]);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// A pointer list: pItem at 0, pNext at 4, for the single- and the
// double-linked kind alike.
void TestList(Query& q, std::uintptr_t head) {
  std::uint32_t node = U32(head);
  for (int n = 0; n < kMaxListNodes && Plausible(node); ++n) {
    TestEntity(q, U32(node));
    if (q.hit && !q.nearest) return;
    node = U32(node + 4);
  }
}

// One sector: the static things out of our own index, the moving ones out
// of the game's repeat sectors.
void TestSector(Query& q, int sx, int sy, const std::uintptr_t* bucket, int count) {
  for (int i = 0; i < count; ++i) {
    TestEntity(q, bucket[i]);
    if (q.hit && !q.nearest) return;
  }
  if (!q.objects && !q.vehicles) return;
  const std::uintptr_t repeat =
      At(kRepeatSectors) +
      ((sy & (kRepeat - 1)) * kRepeat + (sx & (kRepeat - 1))) * kRepeatSectorSize;
  if (q.objects) TestList(q, repeat + kRepeatObjects);
  if (q.hit && !q.nearest) return;
  if (q.vehicles) TestList(q, repeat + kRepeatVehicles);
}

// One sector under a guard: the lists and the models belong to the
// streamer, and it is changing them while this runs.
bool RunSector(Query* q, int sx, int sy, const std::uintptr_t* bucket, int count) {
  __try {
    TestSector(*q, sx, sy, bucket, count);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---- our own index of the static world ----

int BucketOf(int sx, int sy) {
  const int x = sx - g_window_x0;
  const int y = sy - g_window_y0;
  if (x < 0 || y < 0 || x >= kWindow || y >= kWindow) return -1;
  return y * kWindow + x;
}

// One pool into the buckets. Everything is checked before it is read: this
// walks tens of thousands of entries, and a fault here would be a crash
// rather than a missing answer.
int IndexPool(std::uint32_t pool_pointer, int* slots) {
  std::uint32_t pool = 0;
  if (!asi::mem::Read<std::uint32_t>(At(pool_pointer), &pool) || !Plausible(pool)) return 0;
  std::uint32_t objects = 0, byte_map = 0;
  std::int32_t size = 0;
  if (!asi::mem::Read<std::uint32_t>(pool + kPoolObjects, &objects) ||
      !asi::mem::Read<std::uint32_t>(pool + kPoolByteMap, &byte_map) ||
      !asi::mem::Read<std::int32_t>(pool + kPoolSize, &size))
    return 0;
  if (slots != nullptr) *slots = size;
  if (size <= 0 || size > kMaxPoolSize || !Plausible(objects) || !Plausible(byte_map)) return 0;
  if (!asi::mem::IsReadable(objects, static_cast<std::size_t>(size) * kEntityStride) ||
      !asi::mem::IsReadable(byte_map, static_cast<std::size_t>(size)))
    return 0;

  const std::uint32_t table = g_model_table.load(std::memory_order_relaxed);
  int indexed = 0;
  for (int i = 0; i < size; ++i) {
    if ((U8(byte_map + static_cast<std::uint32_t>(i)) & kSlotEmpty) != 0) continue;
    const std::uintptr_t entity = objects + static_cast<std::uint32_t>(i) * kEntityStride;
    if ((U32(entity + kEntityFlags) & 1) == 0) continue;
    const std::int16_t model = S16(entity + kEntityModel);
    if (model < 0) continue;

    // Where it stands, and how far its collision reaches from there.
    float px = 0, py = 0;
    const std::uint32_t matrix = U32(entity + kEntityMatrix);
    if (Plausible(matrix) && asi::mem::IsReadable(matrix, 0x40)) {
      px = F32(matrix + kPos);
      py = F32(matrix + kPos + 4);
    } else {
      px = F32(entity + kEntityPosition);
      py = F32(entity + kEntityPosition + 4);
    }
    if (!(px == px) || !(py == py)) continue;

    float reach = 0;
    const std::uint32_t info = U32(table + static_cast<std::uint32_t>(model) * 4);
    if (Plausible(info) && asi::mem::IsReadable(info, 0x20)) {
      const std::uint32_t colmodel = U32(info + kModelColModel);
      if (Plausible(colmodel) && asi::mem::IsReadable(colmodel, 0x30)) {
        const V lo = Vec(colmodel + kColBoxMin);
        const V hi = Vec(colmodel + kColBoxMax);
        const float ex = std::fabs(lo.x) > std::fabs(hi.x) ? std::fabs(lo.x) : std::fabs(hi.x);
        const float ey = std::fabs(lo.y) > std::fabs(hi.y) ? std::fabs(lo.y) : std::fabs(hi.y);
        reach = ex > ey ? ex : ey;
        if (!(reach == reach) || reach < 0) reach = 0;
        if (reach > kMaxSpread) reach = kMaxSpread;
      }
    }

    // Into every sector its collision can reach into, so that a query need
    // only look at the sector it is asked about.
    const int x0 = SectorX(px - reach), x1 = SectorX(px + reach);
    const int y0 = SectorY(py - reach), y1 = SectorY(py + reach);
    bool placed = false;
    for (int sy = y0; sy <= y1; ++sy)
      for (int sx = x0; sx <= x1; ++sx) {
        const int bucket = BucketOf(sx, sy);
        if (bucket < 0) continue;
        g_bucket[bucket].push_back(entity);
        placed = true;
      }
    if (placed) ++indexed;
  }
  return indexed;
}

void Rebuild(int cx, int cy) {
  for (int i = 0; i < kWindow * kWindow; ++i) g_bucket[i].clear();
  g_window_x0 = cx - kWindow / 2;
  g_window_y0 = cy - kWindow / 2;
  g_indexed_buildings = IndexPool(kBuildingPool, &g_building_slots);
  g_indexed_dummies   = IndexPool(kDummyPool, &g_dummy_slots);
  g_window_built = true;
  g_window_ms = GetTickCount64();
  if (!g_said_index) {
    g_said_index = true;
    LOG_INFO("collision: the static world comes from the game's own pools ({} and {} "
             "slots) - {} buildings and {} dummies within {} m of the player, sorted "
             "into {} sectors of our own", g_building_slots, g_dummy_slots,
             g_indexed_buildings, g_indexed_dummies,
             static_cast<int>(kWindow / 2 * kSectorMetres), kWindow * kWindow);
    if (g_indexed_buildings == 0)
      LOG_WARN("collision: not one building - the pool is not where this build keeps "
               "it, or its entries are laid out differently");
  }
}

// The index has to cover this point and not be too old: the streamer adds
// and removes buildings as the player moves.
void EnsureWindow(float x, float y) {
  const int sx = SectorX(x), sy = SectorY(y);
  const unsigned long long now = GetTickCount64();
  const bool inside = g_window_built && sx - g_window_x0 >= kWindowMargin &&
                      sy - g_window_y0 >= kWindowMargin &&
                      sx - g_window_x0 < kWindow - kWindowMargin &&
                      sy - g_window_y0 < kWindow - kWindowMargin;
  if (inside && now - g_window_ms < kRebuildEveryMs) return;
  Rebuild(sx, sy);
}

bool ReadModelTable() {
  std::uint32_t table = 0;
  if (!asi::mem::Read<std::uint32_t>(At(kModelTableOperand), &table)) return false;
  if (!Plausible(table)) return false;
  if (table != kModelTableExpected)
    LOG_WARN("collision: the model table is at 0x{:08X}, not 0x{:08X} - a limit "
             "adjuster; using the table the game uses", table, kModelTableExpected);
  g_model_table.store(table);
  return true;
}

// Every sector the segment crosses, in turn.
bool Sweep(Query* q, float ax, float ay, float bx, float by) {
  const int x0 = SectorX(ax < bx ? ax : bx), x1 = SectorX(ax < bx ? bx : ax);
  const int y0 = SectorY(ay < by ? ay : by), y1 = SectorY(ay < by ? by : ay);
  bool ok = true;
  for (int sy = y0; sy <= y1; ++sy)
    for (int sx = x0; sx <= x1; ++sx) {
      const int bucket = BucketOf(sx, sy);
      const int count = bucket >= 0 ? static_cast<int>(g_bucket[bucket].size()) : 0;
      const std::uintptr_t* entities = count > 0 ? g_bucket[bucket].data() : nullptr;
      if (!RunSector(q, sx, sy, entities, count)) ok = false;
      if (q->hit && !q->nearest) return ok;
    }
  return ok;
}

// ---- water, from data/water.dat ----
struct WaterPoly {
  float min_x, min_y, max_x, max_y, level;
};
std::vector<WaterPoly> g_water;
std::atomic<bool> g_water_loaded{false};
std::mutex g_water_mutex;

void LoadWater() {
  std::lock_guard<std::mutex> lock(g_water_mutex);
  if (g_water_loaded.load()) return;
  g_water_loaded.store(true);
  char exe[MAX_PATH] = "";
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string path = exe;
  const std::size_t slash = path.find_last_of("\\/");
  path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) +
         "data\\water.dat";
  std::ifstream in(path);
  if (!in) {
    LOG_WARN("collision: {} could not be read - no water is known", path);
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || !(std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '-'))
      continue;
    std::istringstream words(line);
    std::vector<float> values;
    float value = 0;
    while (words >> value) values.push_back(value);
    // Three or four points of seven numbers each, then a type.
    const std::size_t points = values.size() >= 29 ? 4 : values.size() >= 22 ? 3 : 0;
    if (points == 0) continue;
    WaterPoly poly{1e9f, 1e9f, -1e9f, -1e9f, -1e9f};
    for (std::size_t p = 0; p < points; ++p) {
      const float x = values[p * 7], y = values[p * 7 + 1], z = values[p * 7 + 2];
      if (x < poly.min_x) poly.min_x = x;
      if (y < poly.min_y) poly.min_y = y;
      if (x > poly.max_x) poly.max_x = x;
      if (y > poly.max_y) poly.max_y = y;
      if (z > poly.level) poly.level = z;
    }
    g_water.push_back(poly);
  }
  LOG_INFO("collision: {} water areas from {}", g_water.size(), path);
}

}  // namespace

bool Ready() {
  if (g_checked.load()) return g_ready.load();
  std::uint32_t building_pool = 0;
  const bool ok = asi::mem::Read<std::uint32_t>(At(kBuildingPool), &building_pool) &&
                  Plausible(building_pool) &&
                  asi::mem::IsReadable(At(kRepeatSectors),
                                       kRepeat * kRepeat * kRepeatSectorSize) &&
                  ReadModelTable();
  g_ready.store(ok);
  g_checked.store(true);
  if (!ok)
    LOG_WARN("collision: the pools or the model table are not where this build keeps "
             "them - the world cannot be read");
  return ok;
}

bool GroundBelow(float x, float y, float z, float* ground_z, bool include_objects) {
  if (!Ready()) return false;
  EnsureWindow(x, y);
  Query q{};
  q.a = V{x, y, z};
  q.b = V{x, y, z - 1000.0f};
  q.nearest = true;
  q.vehicles = false;
  q.objects = include_objects;
  q.best_t = 2.0f;
  g_queries.fetch_add(1, std::memory_order_relaxed);
  Sweep(&q, x, y, x, y);
  g_entities.fetch_add(q.entities, std::memory_order_relaxed);
  g_primitives.fetch_add(q.primitives, std::memory_order_relaxed);
  if (!q.hit) return false;
  *ground_z = z - 1000.0f * q.best_t;
  return true;
}

bool LineClear(const Vec3& a, const Vec3& b, bool vehicles) {
  if (!Ready()) return false;
  EnsureWindow(a.x, a.y);
  Query q{};
  q.a = V{a.x, a.y, a.z};
  q.b = V{b.x, b.y, b.z};
  q.nearest = false;
  q.vehicles = vehicles;
  q.objects = true;
  q.best_t = 2.0f;
  g_queries.fetch_add(1, std::memory_order_relaxed);
  Sweep(&q, a.x, a.y, b.x, b.y);
  g_entities.fetch_add(q.entities, std::memory_order_relaxed);
  g_primitives.fetch_add(q.primitives, std::memory_order_relaxed);
  return !q.hit;
}

bool PaintFootprint(float cx, float cy, float floor_z, float radius, float cell,
                    float z_lo, float z_hi, float inflate,
                    const std::vector<Leaf>& skip_here,
                    const std::vector<Body>& also, Footprint* out,
                    const Floors* floors, bool vehicles) {
  if (!Ready() || out == nullptr || cell <= 0.05f || radius <= 0) return false;
  EnsureWindow(cx, cy);
  const int side = static_cast<int>(std::ceil(radius * 2.0f / cell));
  if (side <= 0 || side > 400) return false;
  out->cell = cell;
  out->side = side;
  out->x0 = std::floor((cx - radius) / cell) * cell;
  out->y0 = std::floor((cy - radius) / cell) * cell;
  out->blocked.assign(static_cast<std::size_t>(side) * side, 0);
  out->entities = out->primitives = out->painted = 0;

  Paint p;
  p.out = out;
  p.skip = &skip_here;
  p.z_lo = floor_z + z_lo;
  p.z_hi = floor_z + z_hi;
  p.inflate = inflate;
  p.x1 = out->x0 + side * cell;
  p.y1 = out->y0 + side * cell;
  p.budget = 4000000;
  p.floors = floors;
  p.vehicles = vehicles;
  p.band_lo = z_lo;
  p.band_hi = z_hi;
  p.floor_default = floor_z;
  p.floor_min = p.floor_max = floor_z;
  if (floors != nullptr && floors->z != nullptr) {
    // The lowest and highest floor over the square, for the early outs.
    for (int cy = 0; cy < side; ++cy)
      for (int cx = 0; cx < side; ++cx) {
        const float fl = FloorAt(p, out->x0 + (cx + 0.5f) * cell, out->y0 + (cy + 0.5f) * cell);
        p.floor_min = fl < p.floor_min ? fl : p.floor_min;
        p.floor_max = fl > p.floor_max ? fl : p.floor_max;
      }
  }

  // Every sector the square touches, and one all round for things whose
  // collision reaches in from next door.
  const int sx0 = SectorX(out->x0) - 1, sx1 = SectorX(p.x1) + 1;
  const int sy0 = SectorY(out->y0) - 1, sy1 = SectorY(p.y1) + 1;
  for (const bool moving : {true, false})
    for (int sy = sy0; sy <= sy1; ++sy)
      for (int sx = sx0; sx <= sx1; ++sx) {
        const int bucket = BucketOf(sx, sy);
        const std::uintptr_t* items = bucket >= 0 ? g_bucket[bucket].data() : nullptr;
        const int count = bucket >= 0 ? static_cast<int>(g_bucket[bucket].size()) : 0;
        PaintSector(&p, sx, sy, items, count, moving);
      }
  // And whoever is standing about. A player in a doorway is as solid as the
  // doorway, and he is the one obstacle that walks off on his own - which is
  // why the map is redrawn rather than remembered.
  for (const Body& body : also) {
    if (body.z + 1.0f < p.floor_min + p.band_lo || body.z - 1.0f > p.floor_max + p.band_hi) continue;
    PaintCircle(p, body.x, body.y, body.radius, body.z - 1.0f, body.z + 1.0f);
    ++out->entities;
  }
  g_queries.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool WaterAt(float x, float y, float* level) {
  if (!g_water_loaded.load()) LoadWater();
  bool found = false;
  float best = -1e9f;
  for (const WaterPoly& p : g_water) {
    if (x < p.min_x || x > p.max_x || y < p.min_y || y > p.max_y) continue;
    if (!found || p.level > best) best = p.level;
    found = true;
  }
  if (found) *level = best;
  return found;
}

void Explain(float x, float y, float z) {
  if (!Ready()) return;
  EnsureWindow(x, y);
  Query q{};
  q.a = V{x, y, z};
  q.b = V{x, y, z - 1000.0f};
  q.nearest = true;
  q.objects = false;
  q.best_t = 2.0f;
  const int sx = SectorX(x), sy = SectorY(y);
  const int bucket = BucketOf(sx, sy);
  const bool ran = Sweep(&q, x, y, x, y);
  LOG_WARN("collision: ground under ({:.1f},{:.1f},{:.1f}) sector ({},{}) {}: {} static "
           "things indexed there, {} buildings and {} dummies in the window; {} entities "
           "with a collision model, {} whose box the line crosses, {} with data, {} "
           "primitives, hit {} at t {:.4f}", x, y, z, sx, sy, ran ? "read" : "FAULTED",
           bucket >= 0 ? static_cast<int>(g_bucket[bucket].size()) : -1,
           g_indexed_buildings, g_indexed_dummies, q.entities, q.boxed, q.with_data,
           q.primitives, q.hit, q.best_t);
  for (int i = 0; i < q.noted; ++i) LOG_WARN("collision:   {}", q.notes[i]);
}

std::string Line() {
  char text[80];
  std::snprintf(text, sizeof(text), " col=%llu/%llu/%llu",
                static_cast<unsigned long long>(g_queries.load()),
                static_cast<unsigned long long>(g_entities.load()),
                static_cast<unsigned long long>(g_primitives.load()));
  return text;
}

}  // namespace gtabot::game::col
