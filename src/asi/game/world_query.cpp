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
std::atomic<bool> g_los_trusted{false};
std::atomic<bool> g_los_refused{false};
std::atomic<bool> g_enabled{false};
// When arming happened, so the stages can advance from it.
std::atomic<unsigned long long> g_armed_ms{0};
constexpr unsigned long long kStageMs = 25000;

// The ceiling. Comfortably above what the background reads and a single plan
// need, and far below the three hundred a second that took the input away.
constexpr int kCallsPerSecond = 120;
std::atomic<unsigned long long> g_second_started{0};
std::atomic<int> g_calls_this_second{0};
std::atomic<int> g_calls_last_second{0};
std::atomic<bool> g_reported_ceiling{false};
std::atomic<int> g_ground_calls{0}, g_los_calls{0}, g_screen_calls{0};

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

void SetEnabled(bool on) {
  const bool was = g_enabled.exchange(on, std::memory_order_acq_rel);
  if (was == on) return;
  if (on) {
    g_armed_ms.store(GetTickCount64(), std::memory_order_release);
    g_ground_calls.store(0);
    g_los_calls.store(0);
    g_screen_calls.store(0);
  }
  LOG_INFO("game calls {}{}", on ? "ENABLED" : "disabled",
           on ? " - stage 1, ground only" : "");
}

Stage CurrentStage() {
  const unsigned long long armed = g_armed_ms.load(std::memory_order_acquire);
  if (armed == 0) return Stage::kGroundOnly;
  const unsigned long long elapsed = GetTickCount64() - armed;
  if (elapsed < kStageMs) return Stage::kGroundOnly;
  if (elapsed < kStageMs * 2) return Stage::kAndLineOfSight;
  return Stage::kAndScreen;
}

const char* StageName() {
  switch (CurrentStage()) {
    case Stage::kGroundOnly:     return "1:ground";
    case Stage::kAndLineOfSight: return "2:+lineofsight";
    default:                     return "3:+screen";
  }
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

int GroundCalls()      { return g_ground_calls.load(); }
int LineOfSightCalls() { return g_los_calls.load(); }
int ScreenCalls()      { return g_screen_calls.load(); }

int CallsInLastSecond() { return g_calls_last_second.load(); }
int CallsPerSecondCeiling() { return kCallsPerSecond; }

bool GroundBelow(const Vec3& at, float* ground_z) {
  if (!CallsTrusted()) return false;
  if (!TakeCallSlot()) return false;
  g_ground_calls.fetch_add(1, std::memory_order_relaxed);
  const auto fn = reinterpret_cast<FindGroundFn>(At(kFindGroundZFor3DCoord));
  return fn != nullptr && CallGround(fn, at.x, at.y, at.z, ground_z);
}

namespace {
bool RawLineClear(const Vec3& a, const Vec3& b, bool* clear) {
  const auto fn = reinterpret_cast<LineClearFn>(At(kGetIsLineOfSightClear));
  return fn != nullptr && CallLineClear(fn, &a, &b, clear);
}
}  // namespace

bool LineOfSightTrusted() {
  return g_los_trusted.load(std::memory_order_acquire);
}

bool LineOfSightAvailable() {
  return CallsTrusted() && g_los_trusted.load(std::memory_order_acquire) &&
         CurrentStage() >= Stage::kAndLineOfSight;
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
  const auto ground_fn =
      reinterpret_cast<FindGroundFn>(At(kFindGroundZFor3DCoord));
  if (ground_fn == nullptr ||
      !CallGround(ground_fn, player.x, player.y, player.z + 1.0f, &ground)) {
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

bool LineClear(const Vec3& a, const Vec3& b) {
  if (!CallsTrusted()) return false;
  if (!g_los_trusted.load(std::memory_order_acquire)) return false;
  if (CurrentStage() < Stage::kAndLineOfSight) return false;
  if (!TakeCallSlot()) return false;
  g_los_calls.fetch_add(1, std::memory_order_relaxed);
  bool clear = false;
  return RawLineClear(a, b, &clear) && clear;
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
  if (!CallsTrusted()) return false;
  if (CurrentStage() < Stage::kAndScreen) return false;
  if (!TakeCallSlot()) return false;
  g_screen_calls.fetch_add(1, std::memory_order_relaxed);
  const auto fn = reinterpret_cast<ScreenFn>(At(kCalcScreenCoors));
  if (fn == nullptr) return false;
  Vec3 screen;
  if (!CallScreen(fn, &world, &screen)) return false;
  *sx = screen.x;
  *sy = screen.y;
  return true;
}

}  // namespace gtabot::game
