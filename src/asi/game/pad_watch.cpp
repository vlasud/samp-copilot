#include "game/pad_watch.hpp"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "game/exe.hpp"
#include "game/mouse_watch.hpp"
#include "hooks/windowmode.hpp"
#include "log.hpp"
#include "state/memory.hpp"
#include "samp/input_state.hpp"
#include "ui/overlay.hpp"

namespace gtabot::game {
namespace {

// CPad::Update(int pad), thiscall. Its first five bytes are three whole
// instructions (sub esp,30h; push ebx; push ebp), so the trampoline is
// clean.
constexpr std::uint32_t kPadUpdate = 0x541C40;
constexpr std::uint32_t kPads      = 0xB73458;
// The four CControllerStates inside a CPad that Update reconciles.
constexpr std::uint32_t kNewState         = 0x00;
constexpr std::uint32_t kPCTempKeyState   = 0x78;
constexpr std::uint32_t kPCTempJoyState   = 0xA8;
constexpr std::uint32_t kPCTempMouseState = 0xD8;
constexpr std::uint32_t kStateSize        = 0x30;
// Inside a CControllerState.
constexpr std::uint32_t kLeftStickX  = 0x00;
constexpr std::uint32_t kLeftStickY  = 0x02;
constexpr std::uint32_t kRightStickX = 0x04;
constexpr std::uint32_t kRightStickY = 0x06;
constexpr std::uint32_t kDPadUp      = 0x10;
constexpr std::uint32_t kDPadDown    = 0x12;
constexpr std::uint32_t kDPadLeft    = 0x14;
constexpr std::uint32_t kDPadRight   = 0x16;
constexpr std::uint32_t kLeftShoulder1  = 0x08;
constexpr std::uint32_t kRightShoulder1 = 0x0C;
constexpr std::uint32_t kSelect         = 0x1A;
constexpr std::uint32_t kButtonSquare   = 0x1C;
constexpr std::uint32_t kButtonTriangle = 0x1E;
constexpr std::uint32_t kButtonCross    = 0x20;
constexpr std::uint32_t kButtonCircle   = 0x22;
constexpr std::uint32_t kShockButtonL   = 0x24;
// Keys held this long with not one WM_KEYDOWN reaching the window are keys
// the window is not being given at all.
constexpr unsigned long long kLockAfterMs = 700;
constexpr std::uint32_t kDisableControls = 0x10E;
// The keyboard as the game last copied it, and the gates the game's own
// keyboard-to-pad conversion checks.
constexpr std::uint32_t kNewKeyState  = 0xB73190;
constexpr std::uint32_t kTempKeyState = 0xB72CB0;
constexpr std::uint32_t kStandardKeys = 0x18;
constexpr std::uint32_t kLShift       = 0x218 + 34 * 2;
constexpr std::uint32_t kPadNumber    = 0xB73400;
constexpr std::uint32_t kMenuActive   = 0xBA67A4;
constexpr std::uint32_t kPlayers      = 0xB7CD98;
constexpr std::uint32_t kPedFlags     = 0x474;
constexpr std::uint32_t kPedState     = 0x530;
constexpr std::uint32_t kFlagInVehicle = 0x100;
constexpr int           kPedStateDriving = 50;

// The original is thiscall: this in ecx, one argument on the stack, callee
// cleans up. A fastcall with a dummy edx has exactly that shape.
using UpdateFn = void(__fastcall*)(void* pad, void* unused, int number);
UpdateFn g_original = nullptr;
void*    g_target   = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_ignore{true};
std::atomic<bool> g_fallback_on{true};
std::atomic<unsigned long long> g_frames{0};
std::atomic<unsigned long long> g_spoke{0};
std::atomic<unsigned long long> g_fallback_frames{0};
bool g_said = false;
bool g_fallback_said = false;

std::mutex g_mutex;
PadPicture g_last;

short Short(std::uintptr_t at) { return *reinterpret_cast<const short*>(at); }

bool KeyDown(std::uintptr_t keystate, int code) {
  return Short(keystate + kStandardKeys + code * 2) != 0;
}

// Writes the stick from the keyboard when the game did not. Returns whether
// it did.
bool KeyboardFallback(std::uintptr_t pad, PadPicture& picture) {
  if (!g_fallback_on.load(std::memory_order_relaxed)) return false;
  const std::uintptr_t keys = At(kNewKeyState);
  const bool w = KeyDown(keys, 'W'), s = KeyDown(keys, 'S');
  const bool a = KeyDown(keys, 'A'), d = KeyDown(keys, 'D');
  const short want_x = a == d ? 0 : (a ? -128 : 128);
  const short want_y = w == s ? 0 : (w ? -128 : 128);
  if (want_x == 0 && want_y == 0) return false;
  if (picture.new_x != 0 || picture.new_y != 0) return false;   // the game did it
  if (samp::InputLegitimatelyOff(nullptr)) return false;        // and meant not to

  // The game's own reasons to leave the stick alone.
  const bool controls_off = Short(pad + kDisableControls) != 0;
  const bool menu  = *reinterpret_cast<const std::uint8_t*>(At(kMenuActive)) != 0;
  const bool pad2  = *reinterpret_cast<const std::uint8_t*>(At(kPadNumber)) != 0;
  std::uint32_t ped = 0;
  std::uint32_t flags = 0;
  int state = -1;
  asi::mem::Read<std::uint32_t>(At(kPlayers), &ped);
  if (ped != 0) {
    asi::mem::Read<std::uint32_t>(ped + kPedFlags, &flags);
    asi::mem::Read<int>(ped + kPedState, &state);
  }
  const bool in_vehicle = (flags & kFlagInVehicle) != 0 || state == kPedStateDriving;
  if (!g_fallback_said) {
    g_fallback_said = true;
    LOG_WARN("keyboard fallback: the game left the stick empty with keys down "
             "(W{} A{} S{} D{}). Gates: controls_off={} menu={} pad2={} ped=0x{:08X} "
             "in_vehicle={} state={} - {}",
             w ? "+" : "-", a ? "+" : "-", s ? "+" : "-", d ? "+" : "-",
             controls_off, menu, pad2, ped, in_vehicle, state,
             (controls_off || menu || pad2 || in_vehicle || ped == 0)
                 ? "one of them holds, so the game is right and nothing is written"
                 : "none holds, so the stick is written from the keyboard from now on");
  }
  if (controls_off || menu || pad2 || in_vehicle || ped == 0) return false;

  *reinterpret_cast<short*>(pad + kLeftStickX) = want_x;
  *reinterpret_cast<short*>(pad + kLeftStickY) = want_y;
  if (KeyDown(keys, 0x20)) *reinterpret_cast<short*>(pad + kButtonCross) = 255;
  if (Short(keys + kLShift) != 0) *reinterpret_cast<short*>(pad + kButtonSquare) = 255;
  g_fallback_frames.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// The second fallback, for when the window itself gets nothing.
//
// The lock as it is actually seen: the window active and focused, the game
// running, the hand on the keys - and not a single key message arriving at
// the window procedure, so the game's own key table stays empty. What still
// works then is GetAsyncKeyState, which asks the system rather than the
// window. So when movement keys are held for most of a second and the head
// of the window-procedure chain has counted no key-down in that time, the
// pad is written from the keys themselves - movement, sprint, jump, fire,
// aim, enter, crouch, action, camera - until the messages come back.
bool Async(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool AsyncFallback(std::uintptr_t pad) {
  if (!g_fallback_on.load(std::memory_order_relaxed)) return false;
  if (samp::InputLegitimatelyOff(nullptr)) return false;
  static unsigned long long held_since = 0;
  static unsigned long long last_head = 0;
  static bool locked = false;
  // Only the keys the game's own table does not have. A held key the table
  // knows about arrived; its repeats stopping is Windows repeating only the
  // last key pressed, not a lock.
  const std::uintptr_t temp = At(kTempKeyState);
  const auto missing = [&](int vk) { return Async(vk) && !KeyDown(temp, vk); };
  const bool any = missing('W') || missing('A') || missing('S') || missing('D');
  const unsigned long long head = asi::Overlay::KeyMessages();
  const unsigned long long now = GetTickCount64();
  HWND window = GameWindow();
  if (!any || window == nullptr || GetForegroundWindow() != window) {
    held_since = 0;
    last_head = head;
    if (locked) {
      locked = false;
      LOG_INFO("keyboard lock: over - the keys are up or the window is not in front");
    }
    return false;
  }
  if (head != last_head) {
    // Messages are flowing; whatever the game does with them is its own
    // business (the chat swallowing them, say).
    last_head = head;
    held_since = now;
    if (locked) {
      locked = false;
      LOG_INFO("keyboard lock: over - key messages reach the window again");
    }
    return false;
  }
  if (held_since == 0) held_since = now;
  if (!locked) {
    if (now - held_since < kLockAfterMs) return false;
    locked = true;
    LOG_ERROR("keyboard lock: keys held for {} ms and not one key-down reached "
              "the window (head count {}). The pad is written from "
              "GetAsyncKeyState until messages return",
              now - held_since, head);
  }
  const bool w = Async('W'), s = Async('S'), a = Async('A'), d = Async('D');
  const short want_x = a == d ? 0 : (a ? -128 : 128);
  const short want_y = w == s ? 0 : (w ? -128 : 128);
  auto set = [pad](std::uint32_t offset, bool on) {
    if (on) *reinterpret_cast<short*>(pad + offset) = 255;
  };
  if (want_x != 0) *reinterpret_cast<short*>(pad + kLeftStickX) = want_x;
  if (want_y != 0) *reinterpret_cast<short*>(pad + kLeftStickY) = want_y;
  set(kButtonCross, Async(VK_SPACE));
  set(kButtonSquare, Async(VK_LSHIFT));
  set(kButtonCircle, Async(VK_LBUTTON));
  set(kRightShoulder1, Async(VK_RBUTTON));
  set(kButtonTriangle, Async('F') || Async(VK_RETURN));
  set(kShockButtonL, Async('C'));
  set(kLeftShoulder1, Async(VK_TAB));
  set(kSelect, Async('V'));
  g_fallback_frames.fetch_add(1, std::memory_order_relaxed);
  return true;
}

int DPad(std::uintptr_t state) {
  return (Short(state + kDPadUp) ? 1 : 0) | (Short(state + kDPadDown) ? 2 : 0) |
         (Short(state + kDPadLeft) ? 4 : 0) | (Short(state + kDPadRight) ? 8 : 0);
}

void __fastcall HookedUpdate(void* pad, void* unused, int number) {
  (void)unused;
  const std::uintptr_t pad0 = At(kPads);
  const std::uintptr_t self = reinterpret_cast<std::uintptr_t>(pad);
  if (self != pad0) {
    g_original(pad, nullptr, number);
    return;
  }

  PadPicture picture;
  picture.seen  = true;
  picture.frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  const std::uintptr_t key   = self + kPCTempKeyState;
  const std::uintptr_t joy   = self + kPCTempJoyState;
  const std::uintptr_t mouse = self + kPCTempMouseState;
  picture.key_x    = Short(key + kLeftStickX);
  picture.key_y    = Short(key + kLeftStickY);
  picture.key_dpad = DPad(key);
  picture.joy_x    = Short(joy + kLeftStickX);
  picture.joy_y    = Short(joy + kLeftStickY);
  picture.joy_rx   = Short(joy + kRightStickX);
  picture.joy_ry   = Short(joy + kRightStickY);
  picture.joy_dpad = DPad(joy);
  picture.mouse_x  = Short(mouse + kLeftStickX);
  picture.mouse_y  = Short(mouse + kLeftStickY);

  bool spoke = false;
  for (std::uint32_t i = 0; i < kStateSize; i += 2) {
    if (Short(joy + i) == 0) continue;
    spoke = true;
    if (i >= 0x08) ++picture.joy_buttons;
  }
  if (spoke) {
    g_spoke.fetch_add(1, std::memory_order_relaxed);
    if (g_ignore.load(std::memory_order_relaxed)) {
      if (!g_said) {
        g_said = true;
        LOG_WARN("gamepad: the game's joystick had something to say - left "
                 "stick {},{} right {},{} dpad {:#x} buttons {} - and it is "
                 "silenced before the keyboard is reconciled with it; write "
                 "gamepad=on in bot.cfg to let it through",
                 picture.joy_x, picture.joy_y, picture.joy_rx, picture.joy_ry,
                 picture.joy_dpad, picture.joy_buttons);
      }
      std::memset(reinterpret_cast<void*>(joy), 0, kStateSize);
      picture.quieted = true;
    }
  }

  g_original(pad, nullptr, number);

  picture.new_x = Short(self + kNewState + kLeftStickX);
  picture.new_y = Short(self + kNewState + kLeftStickY);
  picture.fallback = KeyboardFallback(self, picture);
  if (!picture.fallback) picture.fallback = AsyncFallback(self);
  // The mouse's turn: while DirectInput is dead, the camera's deltas come
  // from the window messages instead. The camera reads them later in the
  // frame, so after the pad is exactly early enough.
  MouseFallbackFrame();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_last = picture;
}

}  // namespace

bool PadWatchInstall() {
  if (g_installed.load()) return true;
  if (!asi::WindowMode::WalkerAllowed()) return false;
  auto* target = reinterpret_cast<void*>(At(kPadUpdate));
  if (target == nullptr) return false;
  g_ignore.store(!asi::WindowMode::GamepadAllowed());
  if (MH_CreateHook(target, &HookedUpdate,
                    reinterpret_cast<void**>(&g_original)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    static bool said = false;
    if (!said) {
      said = true;
      LOG_ERROR("pad watch: could not hook CPad::Update");
    }
    return false;
  }
  g_target = target;
  g_installed.store(true);
  LOG_INFO("pad watch installed on CPad::Update at gta_sa.exe+0x{:X}; the "
           "gamepad is {}", kPadUpdate - 0x400000,
           g_ignore.load() ? "ignored (gamepad=on in bot.cfg to allow it)"
                           : "allowed");
  return true;
}

PadPicture LastPadPicture() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_last;
}

unsigned long long GamepadSpokeFrames() {
  return g_spoke.load(std::memory_order_relaxed);
}

void SetIgnoreGamepad(bool ignore) {
  g_ignore.store(ignore);
  LOG_INFO("gamepad: {}", ignore ? "ignored" : "allowed");
}

bool IgnoreGamepad() { return g_ignore.load(); }

unsigned long long KeyboardFallbackFrames() {
  return g_fallback_frames.load(std::memory_order_relaxed);
}

void SetKeyboardFallback(bool on) {
  g_fallback_on.store(on);
  LOG_INFO("keyboard fallback: {}", on ? "on" : "off");
}

bool KeyboardFallback() { return g_fallback_on.load(); }

}  // namespace gtabot::game
