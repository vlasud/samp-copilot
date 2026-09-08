#include "game/world_query.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>

#include "game/collision.hpp"
#include "game/exe.hpp"
#include "state/memory.hpp"
#include "log.hpp"

namespace gtabot::game {
namespace {

// gta_sa.exe 1.0 US. All cdecl; references arrive as pointers, and bools
// occupy a full argument slot like everything else on this ABI.
constexpr std::uint32_t kFindGroundZFor3DCoord = 0x5696C0;
// FindGroundZFor3DCoord asks buildings and dummies only. The line tests ask
// objects too, so a bench, a planter or a low wall that is an object reads
// as solid at knee height above a ground that ignores it - a jump at every
// metre inside it. The ground is asked the same way as the lines.
constexpr std::uint32_t kProcessVerticalLine   = 0x5674E0;
constexpr std::uint32_t kGetIsLineOfSightClear = 0x56A490;
constexpr std::uint32_t kCalcScreenCoors       = 0x71DA00;
// CWaterLevel::GetWaterLevel(x, y, z, float* level, bool touching, CVector* normals).
constexpr std::uint32_t kGetWaterLevel         = 0x6EB690;

// CPad for the first player, and the member the game consults before it lets
// him move. Not a call - a read - so it needs no self-check beyond the build
// fingerprint.
//
// The offset was wrong, and wrong in the worst way: 0xF6 lands inside
// PCTempMouseState, so every "controls=enabled" this module has ever logged
// was reading mouse bytes and meant nothing at all. The game disabling the
// player's controls was ruled out over and over on the strength of it. The
// declaration puts it at 0x10E, after the two shake fields:
//
//   CControllerState PCTempMouseState;  0x0D8
//   char             Phase;             0x108
//   short            Mode;              0x10A
//   short            ShakeDur;          0x10C
//   unsigned short   DisablePlayerControls; 0x10E   <- a union of flag bits
//
// CPad is 0x134 bytes, which the array stride has to agree with.
constexpr std::uint32_t kPad0                     = 0xB73458;
// The projection is the game's own: CSprite::CalcScreenCoors transforms the
// point by TheCamera's view matrix and divides by the depth, times the size
// of what is drawn. The view matrix already carries the field of view and
// the aspect ratio inside it, so neither is needed here - and neither is a
// guess about which of them the game believes in.
constexpr std::uint32_t kViewMatrix = 0xA04;   // CCamera::m_mViewMatrix
constexpr std::uint32_t kWidth  = 0xC17044;    // RsGlobal.maximumWidth
constexpr std::uint32_t kHeight = 0xC17048;
constexpr std::uint32_t kMatrixRight = 0x00, kMatrixUp = 0x10, kMatrixAt = 0x20,
                        kMatrixPos = 0x30;
// TheCamera, a CPlaceable: matrix pointer where an entity keeps one, and the
// forward row sixteen bytes into that matrix.
constexpr std::uint32_t kTheCamera        = 0xB6F028;
// CCamera::m_fOrientation. Confirmed in the disassembly of
// PlayerControlZelda (0x6883D0): the value at 0xB6F178 is what it subtracts
// from the stick angle.
constexpr std::uint32_t kCameraOrientation = 0x150;
constexpr std::uint32_t kPlaceableMatrix  = 0x14;
constexpr std::uint32_t kMatrixForward    = 0x10;
constexpr std::uint32_t kPadDisablePlayerControls = 0x10E;

using FindGroundFn = float(__cdecl*)(float x, float y, float z, bool* found,
                                     void** entity);
using VerticalLineFn = bool(__cdecl*)(const Vec3* origin, float distance,
                                      void* col_point, void** entity,
                                      bool buildings, bool vehicles, bool peds,
                                      bool objects, bool dummies,
                                      bool see_through, void* poly);
using LineClearFn  = bool(__cdecl*)(const Vec3* from, const Vec3* to,
                                    bool buildings, bool vehicles, bool peds,
                                    bool objects, bool dummies,
                                    bool see_through, bool camera_ignore);
using ScreenFn     = bool(__cdecl*)(const Vec3* world, Vec3* screen, float* w,
                                    float* h, bool check_max, bool check_min);
using WaterFn      = bool(__cdecl*)(float x, float y, float z, float* level,
                                    bool touching, void* normals);

// A ped's origin sits about a metre above his feet. The self-check accepts a
// generous band around that, because the point is to catch a wrong address -
// which returns rubbish - not to measure the model.
constexpr float kPedHeightMin = 0.3f;
constexpr float kPedHeightMax = 1.8f;

std::atomic<bool> g_trusted{false};
std::atomic<bool> g_los_trusted{false};
std::atomic<bool> g_los_refused{false};
std::atomic<bool> g_enabled{false};
// The ceiling on world queries - the calls that walk the game's collision.
//
// Raised, because the reason it was set this low did not survive: the input
// was lost once at six calls a second and kept through three hundred, so rate
// was never what did it. What genuinely bounds these is the planner's own
// wall clock, which will not spend more than a few milliseconds of any frame.
// This is left as a backstop against a runaway rather than as a budget. The
// planner now works a few milliseconds of every frame and can honestly make
// several hundred calls in each, so the backstop sits well above what a
// frame's worth of planning adds up to over a second; a caller that would
// rather wait than be told "no ground" asks CallSlotsLeft() first.
constexpr int kCallsPerSecond = 50000;
std::atomic<unsigned long long> g_second_started{0};
std::atomic<int> g_calls_this_second{0};
std::atomic<int> g_calls_last_second{0};
std::atomic<bool> g_reported_ceiling{false};
std::atomic<int> g_ground_calls{0}, g_los_calls{0}, g_screen_calls{0};

constexpr int kScreenCallsPerSecond = 2000;
std::atomic<unsigned long long> g_screen_second{0};
std::atomic<int> g_screen_this_second{0};

bool TakeScreenSlot() {
  const unsigned long long now = GetTickCount64();
  if (now - g_screen_second.load(std::memory_order_acquire) >= 1000) {
    g_screen_second.store(now, std::memory_order_release);
    g_screen_this_second.store(0);
  }
  return g_screen_this_second.fetch_add(1, std::memory_order_relaxed) <
         kScreenCallsPerSecond;
}

// True while there is still room this second. Rolls the window over itself,
// so no frame hook has to remember to.
bool TakeCallSlot() {
  const unsigned long long now = GetTickCount64();
  const unsigned long long started = g_second_started.load(std::memory_order_acquire);
  if (now - started >= 1000) {
    g_second_started.store(now, std::memory_order_release);
    g_calls_last_second.store(g_calls_this_second.exchange(0));
  }
  if (g_calls_this_second.fetch_add(1, std::memory_order_relaxed) <
      kCallsPerSecond)
    return true;
  if (!g_reported_ceiling.exchange(true))
    LOG_WARN("game calls hit the ceiling of {} a second - the rest of this "
             "second is answered without asking the game", kCallsPerSecond);
  return false;
}

// Each call is guarded. If the executable is what the fingerprint says, none
// of these can fault; if it is not, a fault here becomes "no answer" rather
// than the session. No C++ objects inside, which is what __try requires.
bool CallGround(FindGroundFn fn, float x, float y, float z, float* out) {
  __try {
    bool found = false;
    void* entity = nullptr;
    const float ground = fn(x, y, z, &found, &entity);
    if (!found) return false;
    *out = ground;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Downward from the origin to the absolute height end_z (the game's own
// ground probe passes -1000), buildings, objects and dummies; the collision
// point's z is the ground. CColPoint is 0x2C bytes; the buffer is larger.
bool CallVerticalLine(VerticalLineFn fn, const Vec3* origin, float end_z,
                      float* out) {
  __try {
    unsigned char col_point[64] = {};
    void* entity = nullptr;
    if (!fn(origin, end_z, col_point, &entity, /*buildings=*/true,
            /*vehicles=*/false, /*peds=*/false, /*objects=*/true,
            /*dummies=*/true, /*see_through=*/false, nullptr))
      return false;
    *out = *reinterpret_cast<const float*>(col_point + 8);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool CallLineClear(LineClearFn fn, const Vec3* a, const Vec3* b, bool* clear,
                   bool vehicles) {
  __try {
    *clear = fn(a, b, /*buildings=*/true, vehicles, /*peds=*/false,
                /*objects=*/true, /*dummies=*/true, /*see_through=*/false,
                /*camera_ignore=*/false);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool CallWater(WaterFn fn, float x, float y, float z, float* level) {
  __try {
    // touching=true, or water more than three metres over the point asked
    // about - the lake bed - is reported as no water at all.
    return fn(x, y, z, level, true, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool CallScreen(ScreenFn fn, const Vec3* world, Vec3* screen) {
  __try {
    float w = 0, h = 0;
    return fn(world, screen, &w, &h, /*check_max=*/false, /*check_min=*/false);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

void SetEnabled(bool on) {
  const bool was = g_enabled.exchange(on, std::memory_order_acq_rel);
  if (was == on) return;
  if (on) {
    g_ground_calls.store(0);
    g_los_calls.store(0);
    g_screen_calls.store(0);
  }
  LOG_INFO("game calls {}", on ? "ENABLED" : "disabled");
}

bool Enabled() { return g_enabled.load(std::memory_order_acquire); }

bool CallsTrusted() {
  return g_enabled.load(std::memory_order_acquire) &&
         g_trusted.load(std::memory_order_acquire);
}

bool SelfCheck(const Vec3& player, const char** why) {
  if (!g_enabled.load(std::memory_order_acquire)) {
    *why = "movement is off";
    return false;
  }
  if (g_trusted.load()) {
    *why = "already verified";
    return true;
  }
  if (!col::Ready()) {
    *why = "the world's tables are not where this build keeps them";
    return false;
  }
  static unsigned long long explained_ms = 0;
  float ground = 0;
  if (!col::GroundBelow(player.x, player.y, player.z + 1.0f, &ground)) {
    *why = "no ground reads under the player";
    if (GetTickCount64() - explained_ms > 5000) {
      explained_ms = GetTickCount64();
      col::Explain(player.x, player.y, player.z + 1.0f);
    }
    return false;
  }
  const float height = player.z - ground;
  if (height < kPedHeightMin || height > kPedHeightMax) {
    // In a vehicle, or on something that does not count as ground - or the
    // reading is wrong. Not trusted yet either way; asked again later.
    *why = "the ground read is not a ped's height below him";
    if (GetTickCount64() - explained_ms > 5000) {
      explained_ms = GetTickCount64();
      LOG_WARN("collision: the ground under the player reads {:.2f} m below him", height);
      col::Explain(player.x, player.y, player.z + 1.0f);
    }
    return false;
  }
  g_trusted.store(true, std::memory_order_release);
  LOG_INFO("collision model verified: ground under the player is {:.2f} m below "
           "him, read from the game's own data", height);
  *why = "ground under the player is where he is standing";
  return true;
}

int GroundCalls()      { return g_ground_calls.load(); }
int LineOfSightCalls() { return g_los_calls.load(); }
int ScreenCalls()      { return g_screen_calls.load(); }

int CallsInLastSecond() { return g_calls_last_second.load(); }
int CallsPerSecondCeiling() { return kCallsPerSecond; }

int CallSlotsLeft() {
  const unsigned long long now = GetTickCount64();
  if (now - g_second_started.load(std::memory_order_acquire) >= 1000)
    return kCallsPerSecond;   // the window rolls over on the next call
  const int used = g_calls_this_second.load(std::memory_order_relaxed);
  return used >= kCallsPerSecond ? 0 : kCallsPerSecond - used;
}

bool GroundBelow(const Vec3& at, float* ground_z, bool include_objects) {
  if (!CallsTrusted()) return false;
  if (!TakeCallSlot()) return false;
  g_ground_calls.fetch_add(1, std::memory_order_relaxed);
  return col::GroundBelow(at.x, at.y, at.z, ground_z, include_objects);
}

namespace {
bool RawLineClear(const Vec3& a, const Vec3& b, bool* clear,
                  bool include_vehicles = true) {
  if (!col::Ready()) return false;
  *clear = col::LineClear(a, b, include_vehicles);
  return true;
}
}  // namespace

bool LineOfSightTrusted() {
  return g_los_trusted.load(std::memory_order_acquire);
}

bool LineOfSightAvailable() {
  return CallsTrusted() && g_los_trusted.load(std::memory_order_acquire);
}

bool SelfCheckLineOfSight(const Vec3& player, const char** why) {
  if (g_los_trusted.load()) {
    *why = "already verified";
    return true;
  }
  if (g_los_refused.load()) {
    *why = "refused earlier";
    return false;
  }
  if (!CallsTrusted()) {
    *why = "the ground call is not verified yet";
    return false;
  }
  float ground = 0;
  if (!col::GroundBelow(player.x, player.y, player.z + 1.0f, &ground)) {
    *why = "no ground under the player to measure against";
    return false;
  }

  // Where he is standing is clear from knee to head, or nobody could stand
  // there.
  bool standing_clear = false;
  if (!RawLineClear(Vec3{player.x, player.y, ground + 0.4f},
                    Vec3{player.x, player.y, ground + 1.6f}, &standing_clear)) {
    *why = "the call faulted";
    g_los_refused.store(true);
    return false;
  }
  // And a line from above him to well under the ground is not clear, because
  // the ground is in the way.
  bool through_ground = false;
  if (!RawLineClear(Vec3{player.x, player.y, ground + 4.0f},
                    Vec3{player.x, player.y, ground - 4.0f}, &through_ground)) {
    *why = "the call faulted";
    g_los_refused.store(true);
    return false;
  }

  if (!standing_clear || through_ground) {
    g_los_refused.store(true);
    LOG_ERROR("line of sight refused: where the player stands reads {}, and a "
              "line through the ground reads {}. That is not what a working "
              "line-of-sight test says, so it is not being called - which is "
              "what was taking the player's input away",
              standing_clear ? "clear" : "BLOCKED",
              through_ground ? "CLEAR" : "blocked");
    *why = "it disagrees with where the player is standing";
    return false;
  }

  g_los_trusted.store(true, std::memory_order_release);
  LOG_INFO("line of sight verified: clear where he stands, blocked through "
           "the ground");
  *why = "it agrees with where the player is standing";
  return true;
}

bool LineClear(const Vec3& a, const Vec3& b, bool include_vehicles) {
  if (!CallsTrusted()) return false;
  if (!g_los_trusted.load(std::memory_order_acquire)) return false;
  if (!TakeCallSlot()) return false;
  g_los_calls.fetch_add(1, std::memory_order_relaxed);
  bool clear = false;
  return RawLineClear(a, b, &clear, include_vehicles) && clear;
}

bool WaterLevel(const Vec3& at, float* level) {
  if (!CallsTrusted()) return false;
  if (!TakeCallSlot()) return false;
  return col::WaterAt(at.x, at.y, level);
}

bool ControlsDisabled(bool* disabled) {
  const std::uintptr_t pad = At(kPad0);
  if (pad == 0) return false;
  std::uint16_t value = 0;
  if (!asi::mem::Read<std::uint16_t>(pad + kPadDisablePlayerControls, &value))
    return false;
  *disabled = value != 0;
  return true;
}

bool CameraHeading(float* radians) {
  const std::uintptr_t camera = At(kTheCamera);
  if (camera == 0) return false;
  std::uint32_t matrix = 0;
  if (!asi::mem::Read<std::uint32_t>(camera + kPlaceableMatrix, &matrix) ||
      matrix == 0)
    return false;
  float fx = 0, fy = 0, fz = 0;
  if (!asi::mem::Read<float>(matrix + kMatrixForward + 0, &fx)) return false;
  if (!asi::mem::Read<float>(matrix + kMatrixForward + 4, &fy)) return false;
  if (!asi::mem::Read<float>(matrix + kMatrixForward + 8, &fz)) return false;

  // A real matrix row is a unit vector. Anything else means this is not the
  // matrix, and steering by it would send the character somewhere arbitrary.
  const float length = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (length < 0.9f || length > 1.1f) return false;
  if (fx == 0.0f && fy == 0.0f) return false;   // looking straight down
  *radians = std::atan2(fy, fx);
  return true;
}

bool CameraOrientation(float* radians) {
  const std::uintptr_t camera = At(kTheCamera);
  if (camera == 0) return false;
  float value = 0;
  if (!asi::mem::Read<float>(camera + kCameraOrientation, &value)) return false;
  // An angle in radians, kept within a turn by the game itself.
  if (!(value == value) || value < -7.0f || value > 7.0f) return false;
  *radians = value;
  return true;
}

bool ToScreen(const Vec3& world, float* sx, float* sy) {
  if (!CallsTrusted()) return false;
  // Cheaper than a world query - arithmetic on the camera - but not free, and
  // taking it off the leash entirely let the node overlay make seventeen
  // thousand of these in eleven seconds. Its own allowance, generous enough
  // for everything drawn at any frame rate and still an allowance.
  if (!TakeScreenSlot()) return false;
  g_screen_calls.fetch_add(1, std::memory_order_relaxed);
  const std::uintptr_t view = At(kTheCamera) + kViewMatrix;
  if (At(kTheCamera) == 0 || !asi::mem::IsReadable(view, 0x40)) return false;
  // right, up, at, pos - the basis the point is spread over, exactly as
  // CMatrix::TransformPoint spreads it.
  float m[12];
  const std::uint32_t rows[4] = {kMatrixRight, kMatrixUp, kMatrixAt, kMatrixPos};
  for (int r = 0; r < 4; ++r)
    for (int i = 0; i < 3; ++i)
      if (!asi::mem::Read<float>(view + rows[r] + i * 4, &m[r * 3 + i])) return false;
  std::int32_t width = 0, height = 0;
  if (!asi::mem::Read<std::int32_t>(At(kWidth), &width) ||
      !asi::mem::Read<std::int32_t>(At(kHeight), &height) || width <= 0 || height <= 0)
    return false;

  const float vx = m[0] * world.x + m[3] * world.y + m[6] * world.z + m[9];
  const float vy = m[1] * world.x + m[4] * world.y + m[7] * world.z + m[10];
  const float depth = m[2] * world.x + m[5] * world.y + m[8] * world.z + m[11];
  if (!(depth > 0.1f)) return false;             // behind the camera, or not a number
  const float rd = 1.0f / depth;
  const float x = static_cast<float>(width) * rd * vx;
  const float y = static_cast<float>(height) * rd * vy;
  if (!(x == x) || !(y == y)) return false;
  // Far outside the window is not worth drawing, and is the shape a wrong
  // matrix would take.
  if (x < -8.0f * width || x > 8.0f * width || y < -8.0f * height || y > 8.0f * height)
    return false;
  *sx = x;
  *sy = y;
  return true;
}

}  // namespace gtabot::game
