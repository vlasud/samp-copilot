#include "game/mouse_watch.hpp"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "bridge.hpp"
#include "game/exe.hpp"
#include "hooks/cursor.hpp"
#include "hooks/windowmode.hpp"
#include "log.hpp"
#include "state/memory.hpp"

namespace gtabot::game {
namespace {

// RsGlobal.ps -> psGlobalType: the window at +0, the DirectInput mouse at
// +0x1C (the same object the game's code reaches as 0xC920D8).
constexpr std::uint32_t kPsPointer = 0xC17054;
constexpr std::uint32_t kPsWindow  = 0x00;
constexpr std::uint32_t kPsMouse   = 0x1C;
// diMouseInit(bool exclusive): CreateDevice(GUID_SysMouse), SetDataFormat,
// SetCooperativeLevel(window, FOREGROUND | (exclusive ? EXCLUSIVE :
// NONEXCLUSIVE)), Acquire. cdecl, one byte argument.
constexpr std::uint32_t kDiMouseInit = 0x7469A0;
// CPad::NewMouseControllerState: the deltas the camera turns by.
constexpr std::uint32_t kNewMouseState = 0xB73418;
constexpr std::uint32_t kMouseX = 0x08;
constexpr std::uint32_t kMouseY = 0x0C;
constexpr std::uint32_t kForegroundApp = 0x8D621C;
// IDirectInputDevice8 vtable slots.
constexpr int kSlotAcquire   = 7;
constexpr int kSlotUnacquire = 8;
constexpr int kSlotGetState  = 9;
constexpr int kSlotCoop      = 13;
constexpr DWORD kForegroundNonExclusive = 0x4 | 0x2;
// How long the pointer has to keep moving with DirectInput silent before the
// mouse is called dead, and between rescue steps.
constexpr unsigned long long kDeadMs = 1500;
constexpr int kMinMovesForEvidence = 3;
// The game has to be asking DirectInput at something like a frame rate for
// silence to mean anything; and a rescue step is given this long to work.
constexpr unsigned long long kMinCallsInWindow = 20;
constexpr unsigned long long kStepMs = 3000;

using AcquireFn   = HRESULT(__stdcall*)(void* self);
using UnacquireFn = HRESULT(__stdcall*)(void* self);
using GetStateFn  = HRESULT(__stdcall*)(void* self, DWORD size, void* data);
using CoopFn      = HRESULT(__stdcall*)(void* self, HWND window, DWORD flags);
using MouseInitFn = void(__cdecl*)(bool exclusive);

struct DiMouseState2 {
  long x, y, z;
  unsigned char buttons[8];
};

GetStateFn g_get_state = nullptr;
AcquireFn  g_acquire   = nullptr;
std::atomic<bool> g_installed{false};

// What DirectInput says, counted.
std::atomic<unsigned long long> g_calls{0}, g_ok{0}, g_motion{0}, g_acquires{0};
std::atomic<long> g_last_hr{0}, g_last_acquire_hr{0};
// What the window and the pointer say.
std::atomic<unsigned long long> g_moves{0}, g_raw{0}, g_pointer_moves{0};
// Moves the hand made: a WM_MOUSEMOVE that did not land where the last
// SetCursorPos put the pointer.
std::atomic<unsigned long long> g_real_moves{0};
// DirectInput reported motion since the last fallback frame.
std::atomic<bool> g_motion_this_frame{false};
// The rescue.
std::atomic<bool> g_fallback{false};
std::atomic<unsigned long long> g_fallback_motion{0};
std::atomic<int>  g_rescues{0};

std::uintptr_t Ps() {
  std::uint32_t ps = 0;
  if (!asi::mem::Read<std::uint32_t>(At(kPsPointer), &ps)) return 0;
  return ps;
}

void* DiMouse() {
  const std::uintptr_t ps = Ps();
  if (ps == 0) return nullptr;
  std::uint32_t device = 0;
  if (!asi::mem::Read<std::uint32_t>(ps + kPsMouse, &device)) return nullptr;
  return reinterpret_cast<void*>(device);
}

void** VTable(void* object) {
  void** table = nullptr;
  if (!asi::mem::Read<void**>(reinterpret_cast<std::uintptr_t>(object), &table))
    return nullptr;
  if (!asi::mem::IsReadable(reinterpret_cast<std::uintptr_t>(table), 16 * sizeof(void*)))
    return nullptr;
  return table;
}

std::string ModuleOf(const void* address) {
  char buffer[MAX_PATH + 32];
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      module == nullptr) {
    std::snprintf(buffer, sizeof(buffer), "0x%08X",
                  static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(address)));
    return buffer;
  }
  char path[MAX_PATH] = "";
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* name = std::strrchr(path, '\\');
  name = name ? name + 1 : path;
  std::snprintf(buffer, sizeof(buffer), "%s+0x%X", name,
                static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(address) -
                                      reinterpret_cast<std::uintptr_t>(module)));
  return buffer;
}

HRESULT __stdcall HookedGetState(void* self, DWORD size, void* data) {
  const HRESULT hr = g_get_state(self, size, data);
  if (self != DiMouse()) return hr;
  g_calls.fetch_add(1, std::memory_order_relaxed);
  const long previous = g_last_hr.exchange(hr);
  if (previous != hr) {
    static int said = 0;
    if (said++ < 20)
      LOG_WARN("mouse: GetDeviceState now returns 0x{:08X} (was 0x{:08X})",
               static_cast<unsigned>(hr), static_cast<unsigned>(previous));
  }
  if (FAILED(hr)) return hr;
  g_ok.fetch_add(1, std::memory_order_relaxed);
  if (size >= sizeof(long) * 2 && data != nullptr) {
    const auto* state = static_cast<const DiMouseState2*>(data);
    if (state->x != 0 || state->y != 0) {
      g_motion.fetch_add(1, std::memory_order_relaxed);
      g_motion_this_frame.store(true, std::memory_order_relaxed);
    }
  }
  return hr;
}

HRESULT __stdcall HookedAcquire(void* self) {
  const HRESULT hr = g_acquire(self);
  if (self != DiMouse()) return hr;
  g_acquires.fetch_add(1, std::memory_order_relaxed);
  const long previous = g_last_acquire_hr.exchange(hr);
  if (previous != hr) {
    static int said = 0;
    if (said++ < 20)
      LOG_WARN("mouse: Acquire now returns 0x{:08X} (was 0x{:08X})",
               static_cast<unsigned>(hr), static_cast<unsigned>(previous));
  }
  return hr;
}

// The rescue steps, on the game thread. Plain C inside the guards: an
// object with a destructor is not allowed in a __try block.
struct Reacquired { HRESULT coop = 1; HRESULT acquire = 1; bool faulted = false; };

Reacquired ReacquireGuarded(void* device, void** table, HWND window) {
  Reacquired result;
  __try {
    reinterpret_cast<UnacquireFn>(table[kSlotUnacquire])(device);
    result.coop = reinterpret_cast<CoopFn>(table[kSlotCoop])(device, window,
                                                             kForegroundNonExclusive);
    result.acquire = reinterpret_cast<AcquireFn>(table[kSlotAcquire])(device);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    result.faulted = true;
  }
  return result;
}

bool RecreateGuarded(MouseInitFn init) {
  __try {
    init(false);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void ReacquireOnGameThread() {
  void* device = DiMouse();
  void** table = device ? VTable(device) : nullptr;
  if (table == nullptr) {
    LOG_ERROR("mouse rescue 1: no device to re-acquire");
    return;
  }
  const Reacquired r = ReacquireGuarded(device, table, GameWindow());
  if (r.faulted)
    LOG_ERROR("mouse rescue 1: re-acquiring faulted");
  else
    LOG_WARN("mouse rescue 1: unacquired, cooperative level -> 0x{:08X}, "
             "acquire -> 0x{:08X}", static_cast<unsigned>(r.coop),
             static_cast<unsigned>(r.acquire));
}

void RecreateOnGameThread() {
  const auto init = reinterpret_cast<MouseInitFn>(At(kDiMouseInit));
  if (init == nullptr) return;
  void* before = DiMouse();
  const bool ok = RecreateGuarded(init);
  void* after = DiMouse();
  if (!ok)
    LOG_ERROR("mouse rescue 2: diMouseInit faulted");
  else
    LOG_WARN("mouse rescue 2: the device re-created through the game's own "
             "diMouseInit: 0x{:08X} -> 0x{:08X}",
             static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(before)),
             static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(after)));
}

// Where the pointer is, in the game window's client coordinates, read
// through the untouched GetCursorPos - the panel's hook answers the game,
// not us.
bool PointerInClient(HWND window, POINT* out) {
  POINT p{};
  if (!asi::CursorHook::RealCursorPos(&p)) return false;
  if (!ScreenToClient(window, &p)) return false;
  *out = p;
  return true;
}

}  // namespace

HWND GameWindow() {
  const std::uintptr_t ps = Ps();
  if (ps == 0) return nullptr;
  std::uint32_t window = 0;
  if (!asi::mem::Read<std::uint32_t>(ps + kPsWindow, &window)) return nullptr;
  return reinterpret_cast<HWND>(window);
}

bool MouseWatchInstall() {
  if (g_installed.load()) return true;
  if (!asi::WindowMode::WalkerAllowed()) return false;
  void* device = DiMouse();
  if (device == nullptr) return false;
  void** table = VTable(device);
  if (table == nullptr) return false;
  void* get_state = table[kSlotGetState];
  void* acquire   = table[kSlotAcquire];
  if (MH_CreateHook(get_state, &HookedGetState,
                    reinterpret_cast<void**>(&g_get_state)) != MH_OK ||
      MH_EnableHook(get_state) != MH_OK ||
      MH_CreateHook(acquire, &HookedAcquire,
                    reinterpret_cast<void**>(&g_acquire)) != MH_OK ||
      MH_EnableHook(acquire) != MH_OK) {
    static bool said = false;
    if (!said) {
      said = true;
      LOG_ERROR("mouse watch: could not hook the DirectInput mouse device");
    }
    return false;
  }
  g_installed.store(true);
  LOG_INFO("mouse watch: hooked GetDeviceState ({}) and Acquire ({}) of the "
           "game's DirectInput mouse 0x{:08X}", ModuleOf(get_state),
           ModuleOf(acquire),
           static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(device)));
  return true;
}

void MouseMessageMove(int x, int y) {
  g_moves.fetch_add(1, std::memory_order_relaxed);
  // Synthetic or real? The pointer is put back somewhere every frame by
  // someone; a move that lands exactly there, soon after, is that.
  POINT target{};
  unsigned long long when = 0;
  HWND window = GameWindow();
  if (window != nullptr && asi::CursorHook::LastSetTarget(&target, &when)) {
    POINT here{x, y};
    if (ClientToScreen(window, &here) && GetTickCount64() - when < 200 &&
        here.x >= target.x - 1 && here.x <= target.x + 1 &&
        here.y >= target.y - 1 && here.y <= target.y + 1)
      return;
  }
  g_real_moves.fetch_add(1, std::memory_order_relaxed);
}

void MouseMessageRaw() { g_raw.fetch_add(1, std::memory_order_relaxed); }

void MouseFallbackFrame() {
  if (!g_fallback.load(std::memory_order_relaxed)) return;
  HWND window = GameWindow();
  if (window == nullptr) return;
  RECT client{};
  if (!GetClientRect(window, &client)) return;
  const int cx = (client.right - client.left) / 2;
  const int cy = (client.bottom - client.top) / 2;
  float dx = 0, dy = 0;
  POINT p{};
  if (PointerInClient(window, &p)) {
    dx = static_cast<float>(p.x - cx);
    dy = static_cast<float>(p.y - cy);
  }
  if (dx != 0 || dy != 0) g_fallback_motion.fetch_add(1, std::memory_order_relaxed);
  // DirectInput spoke this frame after all: its deltas stand, ours do not.
  if (g_motion_this_frame.exchange(false)) return;
  // Back to the centre, so the next frame's move is measured from it -
  // unless the panel is holding the pointer for itself.
  if (!asi::CursorHook::freed()) {
    POINT centre{cx, cy};
    if (ClientToScreen(window, &centre)) SetCursorPos(centre.x, centre.y);
  }
  const std::uintptr_t state = At(kNewMouseState);
  if (!asi::mem::IsReadable(state, 0x10)) return;
  *reinterpret_cast<float*>(state + kMouseX) = dx;
  *reinterpret_cast<float*>(state + kMouseY) = dy;
}

void WatchMouse() {
  if (!g_installed.load()) return;
  HWND window = GameWindow();
  if (window == nullptr) return;

  // The pointer itself, polled: evidence of a hand on the mouse that does
  // not depend on a single message reaching the window.
  {
    static POINT last{-1, -1};
    POINT p{};
    if (asi::CursorHook::RealCursorPos(&p)) {
      if ((p.x != last.x || p.y != last.y) && last.x != -1)
        g_pointer_moves.fetch_add(1, std::memory_order_relaxed);
      last = p;
    }
  }

  // Six quarter-second samples make the window the verdict is drawn over.
  struct Sample { unsigned long long calls, ok, motion, moves; };
  static Sample ring[6] = {};
  static int head = 0;
  static bool full = false;
  static int phase = 0;   // 0 healthy, 1..3 after each rescue step
  static unsigned long long phase_ms = 0;

  const Sample now_sample{g_calls.load(), g_ok.load(), g_motion.load(),
                          g_real_moves.load() + g_pointer_moves.load()};
  const Sample oldest = ring[head];
  ring[head] = now_sample;
  head = (head + 1) % 6;
  if (head == 0) full = true;
  if (!full) return;

  const unsigned long long calls  = now_sample.calls - oldest.calls;
  const unsigned long long ok     = now_sample.ok - oldest.ok;
  const unsigned long long motion = now_sample.motion - oldest.motion;
  const unsigned long long moves  = now_sample.moves - oldest.moves;
  const unsigned long long now = GetTickCount64();

  if (motion > 0) {
    if (phase != 0) {
      LOG_INFO("mouse: DirectInput delivers motion again (after rescue step {})",
               phase);
      if (g_fallback.exchange(false))
        LOG_INFO("mouse: the fallback from the pointer is off");
      phase = 0;
    }
    return;
  }

  // Only with the game in front and believing it is: anywhere else a quiet
  // mouse is just a quiet mouse.
  if (GetForegroundWindow() != window) return;
  std::uint8_t foreground = 0;
  if (!asi::mem::Read<std::uint8_t>(At(kForegroundApp), &foreground) || !foreground)
    return;
  if (calls < kMinCallsInWindow) return;   // the game is barely asking
  const bool hand_moving = moves >= static_cast<unsigned long long>(kMinMovesForEvidence);
  if (!hand_moving) return;   // a quiet mouse is just a quiet mouse
  if (now - phase_ms < (phase == 0 ? kDeadMs : kStepMs)) return;
  phase_ms = now;

  switch (phase) {
    case 0:
      LOG_ERROR("mouse dead: in the last 1.5 s the game asked DirectInput {} "
                "times, {} answered ok, last result 0x{:08X}, no motion in any "
                "of them, while the pointer moved {} times. Rescue 1: re-acquire "
                "the device", calls, ok, static_cast<unsigned>(g_last_hr.load()),
                moves);
      asi::Bridge::PostToGameThread([]() { ReacquireOnGameThread(); });
      phase = 1;
      break;
    case 1:
      LOG_ERROR("mouse still dead after re-acquiring ({} calls, {} ok, last "
                "0x{:08X}, {} pointer moves). Rescue 2: re-create the device",
                calls, ok, static_cast<unsigned>(g_last_hr.load()), moves);
      asi::Bridge::PostToGameThread([]() { RecreateOnGameThread(); });
      g_rescues.fetch_add(1);
      phase = 2;
      break;
    case 2:
      LOG_ERROR("mouse still dead after re-creating the device ({} calls, {} "
                "ok, last 0x{:08X}, {} pointer moves). Rescue 3: the camera is "
                "turned by the pointer itself, polled and pinned back to the "
                "centre every frame, until DirectInput speaks again",
                calls, ok, static_cast<unsigned>(g_last_hr.load()), moves);
      g_fallback.store(true);
      phase = 3;
      break;
    default:
      break;
  }
}

std::string MouseWatchLine() {
  char text[224];
  if (!g_installed.load()) return " mouse:unhooked";
  std::snprintf(text, sizeof(text),
                " di=%llu/%llu/%llu acq=%llu hr=0x%08X moves=%llu/%llu/%llu raw=%llu fb=%d/%llu",
                static_cast<unsigned long long>(g_calls.load()),
                static_cast<unsigned long long>(g_ok.load()),
                static_cast<unsigned long long>(g_motion.load()),
                static_cast<unsigned long long>(g_acquires.load()),
                static_cast<unsigned>(g_last_hr.load()),
                static_cast<unsigned long long>(g_moves.load()),
                static_cast<unsigned long long>(g_real_moves.load()),
                static_cast<unsigned long long>(g_pointer_moves.load()),
                static_cast<unsigned long long>(g_raw.load()),
                g_fallback.load() ? 1 : 0,
                static_cast<unsigned long long>(g_fallback_motion.load()));
  return text;
}

bool MouseFallback() { return g_fallback.load(); }
int  MouseRescues() { return g_rescues.load(); }

}  // namespace gtabot::game
