#include "hooks/cursor.hpp"

#include <intrin.h>
#include <MinHook.h>

#include <atomic>

#include "log.hpp"
#include "state/memory.hpp"

namespace gtabot::asi {
namespace {

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);

SetCursorPosFn g_real_set = nullptr;
GetCursorPosFn g_real_get = nullptr;

std::atomic<bool>          g_installed{false};
std::atomic<bool>          g_freed{false};
std::atomic<std::uint64_t> g_suppressed{0};

// Where the game last asked the pointer to be. Read back to it so the delta
// it computes is zero and the camera stays where the player left it.
std::atomic<LONG> g_pinned_x{0};
std::atomic<LONG> g_pinned_y{0};

// Said once per session, because the answer is the whole diagnosis: if the
// pointer still will not move, this line names what is holding it.
std::atomic<bool> g_named_caller{false};

BOOL WINAPI HookedSetCursorPos(int x, int y) {
  if (!g_freed.load(std::memory_order_acquire)) return g_real_set(x, y);

  if (!g_named_caller.exchange(true)) {
    LOG_INFO("the mouse is being recentred by {} - answering its "
             "SetCursorPos while the panel is interactive",
             mem::DescribeAddress(
                 reinterpret_cast<std::uintptr_t>(_ReturnAddress())));
  }
  g_pinned_x.store(x, std::memory_order_release);
  g_pinned_y.store(y, std::memory_order_release);
  g_suppressed.fetch_add(1, std::memory_order_relaxed);
  return TRUE;
}

BOOL WINAPI HookedGetCursorPos(LPPOINT out) {
  if (out == nullptr) return g_real_get(out);
  if (!g_freed.load(std::memory_order_acquire)) return g_real_get(out);

  out->x = g_pinned_x.load(std::memory_order_acquire);
  out->y = g_pinned_y.load(std::memory_order_acquire);
  return TRUE;
}

}  // namespace

bool CursorHook::Install() {
  if (g_installed.load()) return true;

  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) {
    LOG_ERROR("user32.dll is not loaded - the panel cannot take the mouse");
    return false;
  }
  auto* set = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
  auto* get = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos"));
  if (!set || !get) {
    LOG_ERROR("user32 has no SetCursorPos/GetCursorPos to hook");
    return false;
  }

  // MinHook is already up: the frame hook initialises it before anything else
  // runs. Saying so rather than initialising again keeps one owner of it.
  if (MH_CreateHook(set, &HookedSetCursorPos,
                    reinterpret_cast<void**>(&g_real_set)) != MH_OK ||
      MH_CreateHook(get, &HookedGetCursorPos,
                    reinterpret_cast<void**>(&g_real_get)) != MH_OK) {
    LOG_ERROR("could not hook the cursor calls - the panel will fight the "
              "game for the mouse");
    return false;
  }
  if (MH_EnableHook(set) != MH_OK || MH_EnableHook(get) != MH_OK) {
    LOG_ERROR("could not enable the cursor hooks");
    return false;
  }

  g_installed.store(true);
  LOG_INFO("cursor hooks installed (SetCursorPos -> {}, GetCursorPos -> {})",
           mem::DescribeAddress(reinterpret_cast<std::uintptr_t>(set)),
           mem::DescribeAddress(reinterpret_cast<std::uintptr_t>(get)));
  return true;
}

void CursorHook::Uninstall() {
  if (!g_installed.load()) return;
  // Let go of the pointer before the hooks go, so the game is never left with
  // a frozen cursor and no hook to explain it.
  SetFreed(false);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32) {
    if (auto* set = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos")))
      MH_RemoveHook(set);
    if (auto* get = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos")))
      MH_RemoveHook(get);
  }
  g_installed.store(false);
}

void CursorHook::SetFreed(bool freed) {
  if (g_freed.load() == freed) return;
  if (freed && g_real_get) {
    // Start from where the pointer actually is. Answering the game with a
    // stale point would hand it one enormous delta and swing the camera.
    POINT now{};
    if (g_real_get(&now)) {
      g_pinned_x.store(now.x);
      g_pinned_y.store(now.y);
    }
  }
  g_freed.store(freed, std::memory_order_release);
}

bool CursorHook::freed() { return g_freed.load(std::memory_order_acquire); }

bool CursorHook::RealCursorPos(POINT* out) {
  if (out == nullptr) return false;
  if (g_real_get) return g_real_get(out) != FALSE;
  return GetCursorPos(out) != FALSE;
}

bool CursorHook::installed() { return g_installed.load(); }

std::uint64_t CursorHook::suppressed() {
  return g_suppressed.load(std::memory_order_relaxed);
}

}  // namespace gtabot::asi
