#include "samp/input_state.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "bridge.hpp"
#include "game/exe.hpp"
#include "game/mouse_watch.hpp"
#include "log.hpp"
#include "samp/chat.hpp"
#include "samp/version.hpp"
#include "samp/world.hpp"
#include "state/memory.hpp"

namespace gtabot::samp {
namespace {

// samp.dll 0.3.7-R1.
constexpr std::uint32_t kGamePointerRva      = 0x21A10C;   // CGame* pGame
constexpr std::uint32_t kSetCursorModeRva    = 0x9BD30;    // thiscall (int mode, bool immediate)
constexpr std::uint32_t kProcessEnablingRva  = 0x9BC10;    // thiscall ()
constexpr std::uint32_t kCursorMode  = 0x55;
constexpr std::uint32_t kEnableDelay = 0x59;
constexpr std::uint32_t kGateRva     = 0x21A130;
// CLocalPlayer.
constexpr std::uint32_t kActive        = 0x0C;
constexpr std::uint32_t kWasted        = 0x10;
constexpr std::uint32_t kCleared       = 0x146;
constexpr std::uint32_t kReturnToClass = 0x2FA;
// gta_sa.exe: what SA-MP overwrites, and what belongs there.
constexpr std::uint32_t kKeyboardCall = 0x541DF5;
constexpr std::uint8_t  kKeyboardCallBytes[5] = {0xE8, 0x46, 0xF3, 0xFE, 0xFF};
constexpr std::uint32_t kMouseCall = 0x53F417;
constexpr std::uint8_t  kMouseCallBytes[5] = {0xE8, 0xB4, 0x7A, 0x20, 0x00};
constexpr std::uint32_t kHandler = 0x6194A0;
constexpr std::uint8_t  kHandlerByte = 0xE9;
// How long the player has to hold a movement key with the input off before
// it is put back, and the least time between two such rescues.
constexpr unsigned long long kHeldMs = 1500;
constexpr unsigned long long kRescueGapMs = 3000;

using SetCursorModeFn = void(__fastcall*)(void* self, void* unused, int mode, bool immediate);
using ProcessFn       = void(__fastcall*)(void* self, void* unused);

std::atomic<int> g_rescues{0};

std::uintptr_t GamePointer() {
  const Client client = Detect();
  if (client.base == 0) return 0;
  std::uint32_t game = 0;
  if (!asi::mem::Read<std::uint32_t>(client.base + kGamePointerRva, &game)) return 0;
  return game;
}

bool BytesAre(std::uint32_t address, const std::uint8_t* expected, std::size_t count) {
  std::uint8_t bytes[8] = {};
  if (asi::mem::ReadGuarded(game::At(address), bytes, count) != count) return true;
  return std::memcmp(bytes, expected, count) == 0;
}

bool Async(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Plain C inside the guard: nothing with a destructor.
bool CallGuarded(SetCursorModeFn set_mode, ProcessFn process, void* game) {
  __try {
    set_mode(game, nullptr, 0, true);
    process(game, nullptr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

// The last lines of the SA-MP chat, for the moment the input changes hands:
// a server that freezes, kicks or unspawns a player usually says so there.
std::string ChatTail() {
  if (!CachedChat().valid) return "(chat not resolved)";
  const json chat = ReadChat(4);
  std::string out;
  for (const json& line : chat.value("lines", json::array())) {
    const std::string from = line.value("from", std::string{});
    const std::string text = line.value("text", std::string{});
    if (!out.empty()) out += " | ";
    out += from.empty() ? text : from + ": " + text;
  }
  return out.empty() ? "(chat empty)" : out;
}

InputSwitch ReadInputSwitch() {
  InputSwitch s;
  const std::uintptr_t game = GamePointer();
  if (game == 0) return s;
  const Client client = Detect();
  if (!asi::mem::Read<int>(game + kCursorMode, &s.mode)) return s;
  if (!asi::mem::Read<int>(game + kEnableDelay, &s.delay)) return s;
  s.valid = true;
  asi::mem::Read<int>(client.base + kGateRva, &s.gate);
  if (const std::uintptr_t player = LocalPlayerObject()) {
    s.player_known = asi::mem::Read<int>(player + kActive, &s.active) &&
                     asi::mem::Read<int>(player + kWasted, &s.wasted) &&
                     asi::mem::Read<int>(player + kCleared, &s.cleared) &&
                     asi::mem::Read<int>(player + kReturnToClass, &s.return_to_class);
  }
  s.keyboard_off = !BytesAre(kKeyboardCall, kKeyboardCallBytes, 5);
  s.mouse_off    = !BytesAre(kMouseCall, kMouseCallBytes, 5);
  s.handler_off  = !BytesAre(kHandler, &kHandlerByte, 1);
  return s;
}

void ForceInputOn(const char* why) {
  const Client client = Detect();
  const std::uintptr_t game = GamePointer();
  if (client.base == 0 || game == 0) return;
  const InputSwitch before = ReadInputSwitch();
  const auto set_mode = reinterpret_cast<SetCursorModeFn>(client.base + kSetCursorModeRva);
  const auto process  = reinterpret_cast<ProcessFn>(client.base + kProcessEnablingRva);
  const bool ok = CallGuarded(set_mode, process, reinterpret_cast<void*>(game));
  const InputSwitch after = ReadInputSwitch();
  g_rescues.fetch_add(1);
  if (!ok) {
    LOG_ERROR("samp input: SetCursorMode(0) faulted ({})", why);
    return;
  }
  LOG_WARN("samp input: {} - SetCursorMode(0, true) + ProcessInputEnabling: "
           "cursor mode {} -> {}, keyboard {} -> {}, mouse {} -> {}, handler {} -> {}",
           why, before.mode, after.mode, before.keyboard_off ? "OFF" : "on",
           after.keyboard_off ? "OFF" : "on", before.mouse_off ? "OFF" : "on",
           after.mouse_off ? "OFF" : "on", before.handler_off ? "OFF" : "on",
           after.handler_off ? "OFF" : "on");
}

void WatchSampInput() {
  static InputSwitch last;
  static bool had = false;
  static unsigned long long off_since = 0;
  static unsigned long long held_since = 0;
  static unsigned long long last_rescue_ms = 0;

  const InputSwitch s = ReadInputSwitch();
  if (!s.valid) return;
  const unsigned long long now = GetTickCount64();

  const bool gate_closed = s.gate > 10;
  const bool last_gate_closed = last.gate > 10;
  const bool player_changed = s.player_known && last.player_known &&
                              (s.active != last.active || s.wasted != last.wasted ||
                               s.cleared != last.cleared ||
                               s.return_to_class != last.return_to_class);
  if (!had || s.mode != last.mode || s.keyboard_off != last.keyboard_off ||
      s.mouse_off != last.mouse_off || s.handler_off != last.handler_off ||
      gate_closed != last_gate_closed || player_changed) {
    const bool off = s.keyboard_off || s.mouse_off;
    if (gate_closed != last_gate_closed && had)
      LOG_ERROR("samp input: the window gate samp.dll+0x21A130 is now {} ({}); "
                "local player active={} wasted={} cleared={} return_to_class={}; "
                "chat: {}", s.gate,
                gate_closed ? "CLOSED - the window procedure passes nothing on"
                            : "open",
                s.active, s.wasted, s.cleared, s.return_to_class, ChatTail());
    if (player_changed)
      LOG_WARN("samp input: local player active {} -> {}, wasted {} -> {}, cleared "
               "to spawn {} -> {}, return-to-class {} -> {}; chat: {}", last.active,
               s.active, last.wasted, s.wasted, last.cleared, s.cleared,
               last.return_to_class, s.return_to_class, ChatTail());
    if (off && !(last.keyboard_off || last.mouse_off))
      LOG_WARN("samp input: SA-MP switched the game's input OFF - cursor mode {}, "
               "delay {}, keyboard {}, mouse {}, handler {}, gate {}; local player "
               "active={} wasted={} cleared={} return_to_class={}; chat: {}",
               s.mode, s.delay, s.keyboard_off ? "OFF" : "on",
               s.mouse_off ? "OFF" : "on", s.handler_off ? "OFF" : "on", s.gate,
               s.active, s.wasted, s.cleared, s.return_to_class, ChatTail());
    else if (off)
      LOG_WARN("samp input: still off - cursor mode {}, delay {}, keyboard {}, "
               "mouse {}, handler {}, gate {}", s.mode, s.delay,
               s.keyboard_off ? "OFF" : "on", s.mouse_off ? "OFF" : "on",
               s.handler_off ? "OFF" : "on", s.gate);
    else if (had)
      LOG_INFO("samp input: the game's input is back on (cursor mode {}, delay {})",
               s.mode, s.delay);
    else
      LOG_INFO("samp input: watching - cursor mode {}, delay {}, keyboard {}, "
               "mouse {}", s.mode, s.delay, s.keyboard_off ? "OFF" : "on",
               s.mouse_off ? "OFF" : "on");
  }
  had = true;
  last = s;

  if (!s.keyboard_off && !s.mouse_off) {
    off_since = 0;
    held_since = 0;
    return;
  }
  if (off_since == 0) off_since = now;

  // Reported, not undone. With SA-MP's own cursor mode at zero - no
  // dialog, no chat line, no text-draw wanting the cursor - the input being
  // off is not a state SA-MP's dialogs left behind but one its client
  // protection put there, and putting it back from in here is not this
  // module's business. The player's own F6 then Esc is the player's. What
  // this does is say so once, with the time and the state, so the episode
  // has a line in the log.
  HWND window = game::GameWindow();
  const bool in_front = window != nullptr && GetForegroundWindow() == window;
  const bool held = in_front && (Async('W') || Async('A') || Async('S') || Async('D'));
  const bool leftover = s.mode == 0 && s.delay == -1 && now - off_since >= 2000;
  if (!held) held_since = 0;
  else if (held_since == 0) held_since = now;
  const bool asked = held && now - held_since >= kHeldMs;
  if (!leftover && !asked) return;
  if (now - last_rescue_ms < kRescueGapMs) return;
  last_rescue_ms = now;
  held_since = now;

  char why[220];
  std::snprintf(why, sizeof(why),
                "SA-MP has kept the input off for %llu ms (cursor mode %d, delay %d, "
                "gate %d)%s", static_cast<unsigned long long>(now - off_since), s.mode,
                s.delay, s.gate,
                asked ? " while a movement key was held in the active window"
                      : " with nothing of its own wanting the cursor");
  LOG_ERROR("samp input: {} - left as it is; F6 then Esc puts it back by hand", why);
}

bool InputLegitimatelyOff(const char** why) {
  static unsigned long long checked_ms = 0;
  static bool last = false;
  static const char* last_why = "";
  const unsigned long long now = GetTickCount64();
  // Sixty times a second is plenty; the reads are validated, not free.
  if (now - checked_ms >= 16) {
    checked_ms = now;
    last = false;
    last_why = "";
    std::uint8_t menu = 0;
    if (asi::mem::Read<std::uint8_t>(game::At(0xBA67A4), &menu) && menu != 0) {
      last = true;
      last_why = "the game's menu is open";
    }
    short controls_off = 0;
    if (!last && asi::mem::Read<short>(game::At(0xB73458) + 0x10E, &controls_off) &&
        controls_off != 0) {
      last = true;
      last_why = "the server has frozen the player";
    }
    if (!last) {
      const std::uintptr_t g = GamePointer();
      int mode = 0;
      if (g != 0 && asi::mem::Read<int>(g + kCursorMode, &mode) && mode != 0) {
        last = true;
        last_why = "SA-MP has a dialog, chat line or text-draw up";
      }
    }
    if (!last && !BytesAre(kKeyboardCall, kKeyboardCallBytes, 5)) {
      last = true;
      last_why = "SA-MP has taken the keyboard out of the pad";
    }
    if (!last) {
      const std::uintptr_t player = LocalPlayerObject();
      int active = 1;
      if (player != 0 && asi::mem::Read<int>(player + kActive, &active) && active == 0) {
        last = true;
        last_why = "the local player is not spawned";
      }
    }
  }
  if (why != nullptr) *why = last_why;
  return last;
}

std::string InputSwitchLine() {
  const InputSwitch s = ReadInputSwitch();
  if (!s.valid) return " samp=?";
  char text[128];
  std::snprintf(text, sizeof(text),
                " samp=%d/%d gate=%d lp=%d/%d/%d/%d kb:%s mouse:%s hnd:%s r=%d",
                s.mode, s.delay, s.gate, s.active, s.wasted, s.cleared,
                s.return_to_class, s.keyboard_off ? "OFF" : "on",
                s.mouse_off ? "OFF" : "on", s.handler_off ? "OFF" : "on",
                g_rescues.load());
  return text;
}

}  // namespace gtabot::samp
