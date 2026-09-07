#include "game/world_query.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>

#include "game/exe.hpp"
#include "state/memory.hpp"
#include "log.hpp"

namespace gtabot::game {
namespace {

// gta_sa.exe 1.0 US. All cdecl; references arrive as pointers, and bools
// occupy a full argument slot like everything else on this ABI.
constexpr std::uint32_t kFindGroundZFor3DCoord = 0x5696C0;
constexpr std::uint32_t kGetIsLineOfSightClear = 0x56A490;
constexpr std::uint32_t kCalcScreenCoors       = 0x71DA00;

// CPad for the first player, and the member the game consults before it
// lets him move. Not a call - a read, so it needs no self-check beyond the
// build fingerprint.
constexpr std::uint32_t kPad0                   = 0xB73458;
constexpr std::uint32_t kPadDisablePlayerControls = 0xF6;

using FindGroundFn = float(__cdecl*)(float x, float y, float z, bool* found,
                                     void** entity);
using LineClearFn  = bool(__cdecl*)(const Vec3* from, const Vec3* to,
                                    bool buildings, bool vehicles, bool peds,
                                    bool objects, bool dummies,
                                    bool see_through, bool camera_ignore);
using ScreenFn     = bool(__cdecl*)(const Vec3* world, Vec3* screen, float* w,
                                    float* h, bool check_max, bool check_min);

// A ped's origin sits about a metre above his feet. The self-check accepts a
// generous band around that, because the point is to catch a wrong address -
// which returns rubbish - not to measure the model.
constexpr float kPedHeightMin = 0.3f;
constexpr float kPedHeightMax = 1.8f;

std::atomic<bool> g_trusted{false};

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

bool CallLineClear(LineClearFn fn, const Vec3* a, const Vec3* b, bool* clear) {
  __try {
    *clear = fn(a, b, /*buildings=*/true, /*vehicles=*/true, /*peds=*/false,
                /*objects=*/true, /*dummies=*/true, /*see_through=*/false,
                /*camera_ignore=*/false);
    return true;
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

bool CallsTrusted() { return g_trusted.load(std::memory_order_acquire); }

bool SelfCheck(const Vec3& player, const char** why) {
  if (g_trusted.load()) {
    *why = "already verified";
    return true;
  }
  const auto fn = reinterpret_cast<FindGroundFn>(At(kFindGroundZFor3DCoord));
  if (fn == nullptr) {
    *why = "the executable is not the build these addresses are for";
    return false;
  }
  float ground = 0;
  if (!CallGround(fn, player.x, player.y, player.z + 1.0f, &ground)) {
    *why = "the game reports no ground under the player";
    return false;
  }
  const float height = player.z - ground;
  if (height < kPedHeightMin || height > kPedHeightMax) {
    // In a vehicle, or on something the game does not count as ground - or
    // the address is wrong. Not trusted yet either way; asked again later.
    *why = "the ground the game reports is not a ped's height below him";
    return false;
  }
  g_trusted.store(true, std::memory_order_release);
  LOG_INFO("game calls verified: ground under the player is {:.2f} m below "
           "him", height);
  *why = "ground under the player is where he is standing";
  return true;
}

bool GroundBelow(const Vec3& at, float* ground_z) {
  if (!g_trusted.load(std::memory_order_acquire)) return false;
  const auto fn = reinterpret_cast<FindGroundFn>(At(kFindGroundZFor3DCoord));
  return fn != nullptr && CallGround(fn, at.x, at.y, at.z, ground_z);
}

bool LineClear(const Vec3& a, const Vec3& b) {
  if (!g_trusted.load(std::memory_order_acquire)) return false;
  const auto fn = reinterpret_cast<LineClearFn>(At(kGetIsLineOfSightClear));
  bool clear = false;
  return fn != nullptr && CallLineClear(fn, &a, &b, &clear) && clear;
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

bool ToScreen(const Vec3& world, float* sx, float* sy) {
  if (!g_trusted.load(std::memory_order_acquire)) return false;
  const auto fn = reinterpret_cast<ScreenFn>(At(kCalcScreenCoors));
  if (fn == nullptr) return false;
  Vec3 screen;
  if (!CallScreen(fn, &world, &screen)) return false;
  *sx = screen.x;
  *sy = screen.y;
  return true;
}

}  // namespace gtabot::game
