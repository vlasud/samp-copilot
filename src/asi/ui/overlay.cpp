#include "ui/overlay.hpp"

#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <spdlog/common.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "actions/experiments.hpp"
#include "actions/travel.hpp"
#include "actions/walker.hpp"
#include "bridge.hpp"
#include "game/exe.hpp"
#include "game/input_probe.hpp"
#include "game/map_marker.hpp"
#include "game/mouse_watch.hpp"
#include "game/api_trace.hpp"
#include "game/collision.hpp"
#include "game/watchpoint.hpp"
#include "hooks/windowmode.hpp"
#include "game/pad_watch.hpp"
#include "game/paths.hpp"
#include "game/world_query.hpp"
#include "hooks/cursor.hpp"
#include "hooks/frame.hpp"
#include "log.hpp"
#include "mcp/rpc.hpp"
#include "nav/planner.hpp"
#include "samp/chat.hpp"
#include "samp/discovery.hpp"
#include "samp/input_state.hpp"
#include "samp/keys.hpp"
#include "samp/version.hpp"
#include "samp/world.hpp"
#include "state/memory.hpp"
#include "state/plan.hpp"
#include "state/probe.hpp"
#include "ui/status_source.hpp"

// imgui_impl_win32.h keeps this declaration inside an `#if 0` so the header
// does not have to pull in <windows.h>; upstream asks callers to copy it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg,
                                                             WPARAM wparam,
                                                             LPARAM lparam);

namespace gtabot::asi {
namespace {

// F11: F8 is GTA's screenshot key and F9 was taken too.
constexpr int kToggleKey = VK_F11;
// The log column is as tall as the window, so it holds a lot more than the
// fourteen lines it used to get at the bottom of one.
constexpr int   kLogLines   = 60;
constexpr int   kChatLines  = 8;
constexpr float kLeftColumn = 400.0f;
// Rebuilding the status json costs allocations; at 96 fps that is pure waste
// for numbers a human reads. Refresh it four times a second instead.
constexpr unsigned long long kRefreshMs = 250;

// Closed, with a badge in the corner; the player's menu, driven from the
// keyboard while the game keeps the mouse; and the developer's panel, in its
// passive and interactive (mouse taken) forms. One key: F11 opens and closes
// the menu, and steps the developer's panel back down to closed. The
// developer's panel is reached from the menu's last item.
enum class Mode { kClosed, kMenu, kDevPassive, kDevInteractive };

Mode               g_mode        = Mode::kClosed;
bool               g_initialised = false;
bool               g_disabled    = false;
IDirect3DDevice9*  g_device      = nullptr;
HWND               g_window      = nullptr;
bool               g_toggle_down = false;
bool               g_resources_live = false;
WNDPROC            g_previous_wndproc = nullptr;
std::string        g_ini_path;

// ---- the chain of window procedures ----------------------------------------
//
// The game window's procedure is subclassed by more than one module, and the
// order is the order they got there: whoever hooked last is called first and
// decides whether anyone else hears the message at all. This game has such a
// neighbour. The voice chat (vc.asi) hooks after this panel does, and its
// procedure returns without calling the next one whenever its own state is
// not there or one of its handlers claims the message - and from then on
// nothing reaches the game: no keys, no mouse, no WM_SETCURSOR (so the
// Windows arrow appears), no WM_CLOSE (so the cross does nothing), no layout
// change. That was the "input lock".
//
// So this panel keeps itself at the head. When it finds another procedure in
// front of it, it steps back in front, keeping the other in the chain behind
// it; its own older registration stays where it was, further down, and acts
// as a witness. Every quarter second a WM_NULL with a private mark is sent
// through the neighbour's procedure; if the witness does not see it, the
// neighbour is swallowing everything, and messages are routed round it -
// straight to the procedure this panel originally found - until the probe
// gets through again.
WNDPROC g_head_next   = nullptr;   // the procedure that hooked after ours
bool    g_bypass      = false;     // never set any more: routing round SA-MP is not done
bool    g_swallowing  = false;     // the neighbour is returning without passing on
bool    g_probe_seen  = false;
int     g_probe_misses = 0;
unsigned long long g_probe_ms = 0;
constexpr WPARAM kProbe = 0x6774B07D;
constexpr unsigned long long kProbeEveryMs = 250;
std::atomic<unsigned long long> g_keys_head{0};    // key messages at the head
std::atomic<unsigned long long> g_keys_witness{0}; // ... that got past the neighbour
// The messages currently being handled on this thread, so the older copy of
// our procedure can recognise the message the head copy is already handling.
struct MsgFrame { UINT message; WPARAM wparam; LPARAM lparam; };
thread_local MsgFrame g_frames[8];
thread_local int      g_depth = 0;

std::string ModuleOf(const void* address) {
  char buffer[MAX_PATH + 32];
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      module == nullptr) {
    std::snprintf(buffer, sizeof(buffer), "0x%08X (not in any module)",
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

std::string ChainState() {
  if (g_previous_wndproc == nullptr) return "not hooked";
  if (g_head_next == nullptr) return "head";
  return std::string(g_swallowing ? "head over (swallowing) " : "head over ") +
         ModuleOf(g_head_next);
}

const ImVec4 kGreen{0.45f, 0.85f, 0.45f, 1.0f};
const ImVec4 kAmber{0.95f, 0.75f, 0.30f, 1.0f};
const ImVec4 kRed  {0.95f, 0.40f, 0.40f, 1.0f};
const ImVec4 kGrey {0.60f, 0.60f, 0.60f, 1.0f};

// ---- the player's interface ------------------------------------------------
//
// What someone playing with the mod sees: a badge in the corner while the
// panel is closed, and a menu on the keyboard while it is open. No mouse.
// The game keeps the pointer, so opening the menu changes nothing about the
// camera or the cursor, and while it is open the keys it uses are swallowed
// before they reach the game or SA-MP. Everything the developer's panel
// shows is still there, behind the menu's last item.

constexpr float kMenuWidth = 400.0f;
constexpr float kMenuPad   = 18.0f;
constexpr float kRowHeight = 32.0f;
constexpr float kInfoRow   = 25.0f;
constexpr float kBodyPx    = 17.0f;
constexpr float kSmallPx   = 13.5f;
constexpr float kTitlePx   = 21.0f;
constexpr float kMenuTop   = 0.22f;   // of the screen's height
constexpr unsigned long long kMessageMs = 6000;

const ImU32 kUiBg        = IM_COL32(14, 16, 22, 228);
const ImU32 kUiBorder    = IM_COL32(255, 255, 255, 22);
const ImU32 kUiLine      = IM_COL32(255, 255, 255, 14);
const ImU32 kUiAccent    = IM_COL32(255, 176, 46, 255);
const ImU32 kUiAccentDim = IM_COL32(255, 176, 46, 46);
const ImU32 kUiPill      = IM_COL32(255, 255, 255, 20);
const ImU32 kUiPillText  = IM_COL32(20, 20, 24, 255);
const ImU32 kUiText      = IM_COL32(236, 238, 242, 255);
const ImU32 kUiMuted     = IM_COL32(150, 156, 168, 255);
const ImU32 kUiDim       = IM_COL32(104, 110, 122, 255);
const ImU32 kUiOk        = IM_COL32(98, 214, 120, 255);
const ImU32 kUiWarn      = IM_COL32(255, 176, 46, 255);
const ImU32 kUiBad       = IM_COL32(240, 90, 90, 255);

ImFont* g_body  = nullptr;
ImFont* g_bold  = nullptr;
float   g_scale = 1.0f;         // 1 at 1080 lines; everything is drawn in it
bool    g_hud        = true;    // the badge in the corner while closed
bool    g_show_route = true;    // the route drawn on the ground while he walks
int     g_selected   = 0;
std::string        g_message;   // a line for the player, briefly
unsigned long long g_message_until = 0;

void Say(std::string text) {
  g_message       = std::move(text);
  g_message_until = GetTickCount64() + kMessageMs;
}

// The keys the menu is driven with, edge-detected with a repeat, so a held
// arrow scrolls. Primed when the menu opens, so a key that was already down
// - the F11 that opened it, say - does not fire into it.
struct MenuKey {
  int  vk;
  bool repeat;
  bool was  = false;
  unsigned long long next = 0;
};
MenuKey g_keys[] = {
    {VK_UP, true},    {'W', true},       {VK_DOWN, true},  {'S', true},
    {VK_LEFT, false}, {'A', false},      {VK_RIGHT, false}, {'D', false},
    {VK_RETURN, false}, {VK_SPACE, false}, {VK_ESCAPE, false},
};

bool KeyFired(MenuKey& key, unsigned long long now) {
  const bool down = (GetAsyncKeyState(key.vk) & 0x8000) != 0;
  bool fired = false;
  if (down && !key.was) {
    fired = true;
    key.next = now + 380;
  } else if (down && key.repeat && now >= key.next) {
    fired = true;
    key.next = now + 90;
  }
  key.was = down;
  return fired;
}

void PrimeKeys() {
  for (MenuKey& key : g_keys) key.was = (GetAsyncKeyState(key.vk) & 0x8000) != 0;
}

// Any key of the menu's, by its code; several codes may mean one thing.
bool Fired(unsigned long long now, int a, int b = 0) {
  bool fired = false;
  for (MenuKey& key : g_keys)
    if ((key.vk == a || key.vk == b) && KeyFired(key, now)) fired = true;
  return fired;
}

// FrontEndMenuManager.m_bMenuActive: the game's own menu, the map included.
bool PauseMenuOpen() {
  std::uint8_t active = 0;
  return mem::Read<std::uint8_t>(game::At(0xBA67A4), &active) && active != 0;
}

// The one place the mode changes, so the pointer changes hands in step with
// it: only the developer's interactive panel takes the mouse.
void SetMode(Mode mode) {
  if (mode == Mode::kMenu) PrimeKeys();
  g_mode = mode;
  const bool mouse = mode == Mode::kDevInteractive;
  // ImGui draws the cursor itself: the game hides the system one, so there
  // would otherwise be nothing to aim with.
  if (ImGui::GetCurrentContext()) ImGui::GetIO().MouseDrawCursor = mouse;
  CursorHook::SetFreed(mouse);
}

const char* ModeName() {
  switch (g_mode) {
    case Mode::kMenu:           return "menu";
    case Mode::kDevPassive:     return "dev-passive";
    case Mode::kDevInteractive: return "dev-interactive";
    default:                    return "closed";
  }
}

// Results of the posted diagnostics: written by a task on the game thread,
// read by the panel on a later frame.
std::mutex  g_summary_mutex;
std::string g_report_summary;

void SetReportSummary(std::string text) {
  std::lock_guard<std::mutex> lock(g_summary_mutex);
  g_report_summary = std::move(text);
}
std::string ReportSummary() {
  std::lock_guard<std::mutex> lock(g_summary_mutex);
  return g_report_summary;
}

void Label(const char* name, const std::string& value,
           const ImVec4& colour = ImVec4{1, 1, 1, 1}) {
  ImGui::TextColored(kGrey, "%s", name);
  ImGui::SameLine(70.0f);
  ImGui::TextColored(colour, "%s", value.c_str());
}

const ImVec4& LevelColour(int level) {
  if (level >= spdlog::level::err) return kRed;
  if (level == spdlog::level::warn) return kAmber;
  return kGrey;
}

void Teardown() {
  if (!g_initialised) return;
  CursorHook::SetFreed(false);
  if (g_previous_wndproc && g_window) {
    SetWindowLongPtrW(g_window, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_previous_wndproc));
    g_previous_wndproc = nullptr;
  }
  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_body = g_bold = nullptr;
  g_initialised = false;
  g_device      = nullptr;
  g_window      = nullptr;
}

bool IsMouseMessage(UINT message) {
  return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
         message == WM_MOUSEHOVER || message == WM_MOUSELEAVE ||
         message == WM_NCMOUSEMOVE;
}

bool IsKeyboardMessage(UINT message) {
  return message == WM_KEYDOWN || message == WM_KEYUP ||
         message == WM_SYSKEYDOWN || message == WM_SYSKEYUP ||
         message == WM_CHAR;
}

// The handful of messages that decide whether the game believes it has the
// keyboard. WM_KILLFOCUS clears the game's ForegroundApp flag and WM_SETFOCUS
// sets it; WM_ACTIVATE does both and clears the pads. A keyboard that has
// gone quiet with the window still in front is explained by one of these
// arriving without its partner, so each is logged with who it names.
void LogFocusMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  static unsigned long long window_ms = 0;
  static int in_window = 0;
  const unsigned long long now = GetTickCount64();
  if (now - window_ms > 1000) {
    window_ms = now;
    in_window = 0;
  }
  if (++in_window > 12) return;   // a storm is a fact worth one line, not many

  const auto describe = [](HWND other) {
    if (other == nullptr) return std::string("none");
    char cls[64] = "";
    GetClassNameA(other, cls, sizeof(cls));
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(other, &pid);
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "0x%08X(%s tid=%lu%s)",
                  static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(other)),
                  cls, static_cast<unsigned long>(tid),
                  pid == GetCurrentProcessId() ? "" : " other process");
    return std::string(buffer);
  };
  (void)window;
  switch (message) {
    case WM_ACTIVATE:
      LOG_INFO("window: WM_ACTIVATE {} (minimised={}) other={}",
               LOWORD(wparam) == WA_INACTIVE ? "INACTIVE"
               : LOWORD(wparam) == WA_ACTIVE ? "active" : "click-active",
               HIWORD(wparam) != 0, describe(reinterpret_cast<HWND>(lparam)));
      break;
    case WM_ACTIVATEAPP:
      LOG_INFO("window: WM_ACTIVATEAPP {} (other thread {})",
               wparam ? "activated" : "DEACTIVATED",
               static_cast<unsigned long>(lparam));
      break;
    case WM_SETFOCUS:
      LOG_INFO("window: WM_SETFOCUS from {}",
               describe(reinterpret_cast<HWND>(wparam)));
      break;
    case WM_KILLFOCUS:
      LOG_WARN("window: WM_KILLFOCUS - keyboard focus goes to {}",
               describe(reinterpret_cast<HWND>(wparam)));
      break;
    case WM_INPUTLANGCHANGE:
      LOG_INFO("window: WM_INPUTLANGCHANGE layout=0x{:04X}",
               static_cast<unsigned>(LOWORD(lparam)));
      break;
    case WM_CAPTURECHANGED:
      LOG_INFO("window: WM_CAPTURECHANGED to {}",
               describe(reinterpret_cast<HWND>(lparam)));
      break;
    case WM_ENABLE:
      LOG_WARN("window: WM_ENABLE {}", wparam ? "enabled" : "DISABLED");
      break;
    default:
      break;
  }
}

LRESULT CALLBACK HookedWndProc(HWND window, UINT message, WPARAM wparam,
                               LPARAM lparam);

// The head copy's work, once the chain bookkeeping is done.
LRESULT HeadWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

// Window messages only reach ImGui while the panel is interactive, so the game
// keeps its input in every other state.
LRESULT CALLBACK HookedWndProc(HWND window, UINT message, WPARAM wparam,
                               LPARAM lparam) {
  // The probe, wherever it arrives: it got past whoever is in front.
  if (message == WM_NULL && wparam == kProbe) {
    g_probe_seen = true;
    return 0;
  }
  const bool key = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
  // Our older registration, behind the neighbour: the head copy is already
  // handling this very message further up the stack. Only witness it and
  // pass it on to the procedure we originally found.
  for (int i = 0; i < g_depth && i < 8; ++i) {
    if (g_frames[i].message == message && g_frames[i].wparam == wparam &&
        g_frames[i].lparam == lparam) {
      if (key) g_keys_witness.fetch_add(1, std::memory_order_relaxed);
      return CallWindowProcW(g_previous_wndproc, window, message, wparam, lparam);
    }
  }
  if (key) g_keys_head.fetch_add(1, std::memory_order_relaxed);
  if (g_depth < 8) g_frames[g_depth] = MsgFrame{message, wparam, lparam};
  ++g_depth;
  const LRESULT result = HeadWndProc(window, message, wparam, lparam);
  --g_depth;
  return result;
}

LRESULT HeadWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  // The hand on the mouse, as the window sees it: the evidence the mouse
  // watch judges DirectInput against, and the deltas it falls back on.
  if (message == WM_MOUSEMOVE)
    game::MouseMessageMove(static_cast<int>(static_cast<short>(LOWORD(lparam))),
                           static_cast<int>(static_cast<short>(HIWORD(lparam))));
  else if (message == WM_INPUT)
    game::MouseMessageRaw();
  switch (message) {
    case WM_ACTIVATE: case WM_ACTIVATEAPP: case WM_SETFOCUS: case WM_KILLFOCUS:
    case WM_INPUTLANGCHANGE: case WM_CAPTURECHANGED: case WM_ENABLE:
      LogFocusMessage(window, message, wparam, lparam);
      break;
    default:
      break;
  }
  // Losing the focus is news the game does not need.
  //
  // On hearing it, GTA pauses: it stops drawing, and with the drawing goes
  // everything else this module depends on, because all of it runs on the
  // game's own thread once a frame. Somebody clicking on their browser
  // should not stop the character mid-street. So the three messages that
  // carry the news are answered here and go no further; the ones that say
  // the focus has come back are passed on, so the game and the window agree
  // again the moment anybody looks at it.
  if (WindowMode::RunsInBackground()) {
    if (message == WM_ACTIVATEAPP && wparam == FALSE) return 0;
    if (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE) return 0;
    if (message == WM_KILLFOCUS) return 0;
  }

  // A lone Alt is how Windows opens a window's system menu, and while that
  // menu is up the game is not running: no frames, no pad, no packets. The
  // player's own controller table puts several actions on Alt - walking
  // slowly, which is the key nearly every Russian roleplay server watches
  // for as KEY_WALK - so pressing it is not optional. Refusing the menu for
  // as long as a keystroke of ours is in flight is what makes the two
  // compatible, and a person's own Alt, pressed when we are not pressing
  // anything, still opens the menu exactly as before.
  //
  // Nothing here may take a lock. SendInput delivers to this very window on
  // this very thread, so this procedure runs *inside* the call that sends a
  // key - and the key player holds its own mutex across that call. Asking it
  // anything that locks deadlocks the game thread, which stops rendering,
  // which stops everything. The timestamp is an atomic for that reason.
  //
  // The menu is refused whenever it was a bare Alt that asked for it, ours or
  // anybody's. lParam carries the character the menu was opened with and is
  // zero for Alt on its own, so Alt+Space still opens the window menu the way
  // a person expects while Alt as a game key never does. This window has no
  // menu bar and nothing to choose from; what it has is a modal loop that
  // stops the game dead, which is what the game thread was found sitting in.
  if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_KEYMENU &&
      lparam == 0)
    return 0;

  if (g_mode == Mode::kMenu) {
    // The menu is open, so the keyboard is its: nothing pressed reaches the
    // game or SA-MP - not the arrows, not Enter, not Escape. Releases go
    // through, so a key that was held when the menu opened is let go of in
    // the game too; otherwise he would keep running with nobody pressing.
    if (message == WM_KEYDOWN || message == WM_CHAR || message == WM_SYSCHAR ||
        (message == WM_SYSKEYDOWN && wparam != VK_F4))
      return 0;
  }
  if (g_mode == Mode::kDevInteractive && ImGui::GetCurrentContext()) {
    // The panel never sees a key. It has nothing to type into, the game
    // reads its keyboard from these very messages, and keeping ImGui out of
    // that path entirely is the only way to be sure it is not the panel
    // standing between a key and the character.
    if (!IsKeyboardMessage(message)) {
      ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
      const ImGuiIO& io = ImGui::GetIO();
      if (io.WantCaptureMouse && IsMouseMessage(message)) return TRUE;
    }
  }
  // On to the next: the neighbour that hooked after us, unless it is
  // swallowing everything, in which case straight past it.
  const WNDPROC next = (g_head_next != nullptr && !g_bypass) ? g_head_next
                                                              : g_previous_wndproc;
  return CallWindowProcW(next, window, message, wparam, lparam);
}

// Keeps this procedure at the head of the chain and the neighbour honest.
// Game thread, every frame.
void TendWndProcChain(unsigned long long now) {
  if (g_window == nullptr || g_previous_wndproc == nullptr) return;

  // Who is at the head? Asked both ways: a procedure set with the ANSI
  // call is reported to the wide call as a handle, not a pointer.
  const auto wide   = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_window, GWLP_WNDPROC));
  const auto narrow = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(g_window, GWLP_WNDPROC));
  static int  rehooks = 0;
  static unsigned long long rehook_ms = 0;
  if (wide != &HookedWndProc && narrow != &HookedWndProc && wide != nullptr &&
      now - rehook_ms >= 1000) {
    rehook_ms = now;
    // Not a fight. A module that re-hooks every time it is displaced would
    // otherwise have the two of us trading places forever; after a few
    // rounds it keeps the head and we watch from behind it.
    if (++rehooks > 5) {
      if (rehooks == 6)
        LOG_ERROR("window: {} keeps re-hooking the window procedure in front "
                  "of us - leaving it there", ModuleOf(reinterpret_cast<void*>(narrow)));
      return;
    }
    g_head_next = wide;
    g_bypass = false;
    g_probe_misses = 0;
    SetWindowLongPtrW(g_window, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(&HookedWndProc));
    LOG_WARN("window: {} hooked the window procedure after us and now decides "
             "what the game hears; stepping back in front of it (it stays in "
             "the chain, watched)", ModuleOf(reinterpret_cast<void*>(narrow)));
  }
  if (g_head_next == nullptr) return;
  if (now - g_probe_ms < kProbeEveryMs) return;
  g_probe_ms = now;

  g_probe_seen = false;
  CallWindowProcW(g_head_next, g_window, WM_NULL, kProbe, 0);
  if (g_probe_seen) {
    if (g_swallowing) {
      g_swallowing = false;
      LOG_INFO("window: the neighbour passes messages again");
    }
    g_probe_misses = 0;
    return;
  }
  if (++g_probe_misses < 2 || g_swallowing) return;
  g_swallowing = true;
  LOG_ERROR("window: the procedure hooked after ours ({}) swallows every "
            "message - keys, mouse, cursor, close. It is SA-MP's own gate and "
            "it stays in charge; this is only noted",
            ModuleOf(reinterpret_cast<void*>(g_head_next)));
}

std::string IniPathBesideModule() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&IniPathBesideModule), &self);
  char path[MAX_PATH] = {};
  GetModuleFileNameA(self, path, MAX_PATH);
  std::string full(path);
  const std::size_t slash = full.find_last_of("/\\");
  full = slash == std::string::npos ? std::string{} : full.substr(0, slash + 1);
  // Not plain "imgui.ini": vc.asi has an ImGui of its own and writes that file
  // in the same directory.
  return full + "bot.imgui.ini";
}

bool Initialise(IDirect3DDevice9* device) {
  D3DDEVICE_CREATION_PARAMETERS params{};
  if (FAILED(device->GetCreationParameters(&params))) return false;
  g_window = params.hFocusWindow;
  if (!g_window) return false;

  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  g_ini_path      = IniPathBesideModule();
  io.IniFilename  = g_ini_path.c_str();  // remembers where the panel was put
  io.LogFilename  = nullptr;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().WindowRounding = 4.0f;
  ImGui::GetStyle().Alpha          = 0.92f;

  // The built-in font has ASCII glyphs and nothing else, and the interface
  // speaks Russian. Segoe UI ships with every Windows since Vista and covers
  // Cyrillic and the few symbols the menu uses; it is sized to the screen,
  // and that size is the scale everything else is drawn in.
  RECT client{};
  GetClientRect(g_window, &client);
  const float lines = static_cast<float>(client.bottom - client.top);
  g_scale = lines > 0 ? std::clamp(lines / 1080.0f, 0.75f, 2.0f) : 1.0f;
  static ImVector<ImWchar> ranges;   // read while the atlas is built, later
  if (ranges.empty()) {
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    builder.AddText("•↑↓←→›…");
    builder.BuildRanges(&ranges);
  }
  char windir[MAX_PATH] = {};
  if (GetWindowsDirectoryA(windir, MAX_PATH)) {
    const std::string fonts = std::string(windir) + "\\Fonts\\";
    g_body = io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(),
                                          kBodyPx * g_scale, nullptr,
                                          ranges.Data);
    g_bold = io.Fonts->AddFontFromFileTTF((fonts + "segoeuib.ttf").c_str(),
                                          kTitlePx * g_scale, nullptr,
                                          ranges.Data);
  }
  if (g_body == nullptr) {
    LOG_WARN("no Segoe UI in the Windows fonts - the panel falls back to the "
             "built-in font, which cannot draw Cyrillic");
    g_body = io.Fonts->AddFontDefault();
  }
  if (g_bold == nullptr) g_bold = g_body;
  io.FontDefault = g_body;

  if (!ImGui_ImplWin32_Init(g_window)) return false;
  if (!ImGui_ImplDX9_Init(device)) {
    ImGui_ImplWin32_Shutdown();
    return false;
  }

  g_previous_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedWndProc)));
  if (!g_previous_wndproc)
    LOG_WARN("could not chain the window procedure - the panel will not take "
             "the mouse");
  else
    LOG_INFO("window procedure hooked; behind us: {}",
             ModuleOf(reinterpret_cast<void*>(g_previous_wndproc)));
  g_head_next = nullptr;
  g_bypass = false;

  // The panel is the only thing that wants the mouse, so the hooks that take
  // it live and die with the panel.
  CursorHook::Install();

  g_device      = device;
  g_initialised = true;
  LOG_INFO("overlay initialised on hwnd 0x{:08X}, settings in {}",
           reinterpret_cast<std::uintptr_t>(g_window), g_ini_path);
  return true;
}

// True when render target 0 is the swap chain's back buffer rather than one of
// the game's own off-screen surfaces.
bool IsBackBufferBound(IDirect3DDevice9* device) {
  IDirect3DSurface9* target = nullptr;
  if (FAILED(device->GetRenderTarget(0, &target)) || !target) return false;

  IDirect3DSurface9* back = nullptr;
  const bool same =
      SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
      back == target;

  target->Release();
  if (back) back->Release();
  return same;
}

void PollToggle() {
  // F11 alone. With Ctrl it is the arming key, not the panel's.
  const bool down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0 &&
                    !(GetAsyncKeyState(VK_CONTROL) & 0x8000);
  if (down && !g_toggle_down) {
    switch (g_mode) {
      case Mode::kClosed:
        // Not over the game's own menu: the map is where the marker is put.
        if (!PauseMenuOpen()) SetMode(Mode::kMenu);
        break;
      case Mode::kMenu:           SetMode(Mode::kClosed);     break;
      case Mode::kDevInteractive: SetMode(Mode::kDevPassive); break;
      case Mode::kDevPassive:     SetMode(Mode::kClosed);     break;
    }
  }
  g_toggle_down = down;
}

// ---- movement debugging ----------------------------------------------------

// Eight spokes at six metres, refreshed every two seconds: about fifty calls
// a second where the sixteen-spoke version at one second managed three
// hundred, which is the rate that kept taking the player's input away.
constexpr int   kFanSpokes   = 8;
constexpr float kFanMetres   = 6.0f;
constexpr float kNodeRadius  = 50.0f;
// Every one of these is a projection every frame. Three hundred of them at
// ninety frames a second is seventeen thousand calls in eleven seconds, which
// is what the node overlay was actually doing.
constexpr std::size_t kMaxDrawnNodes = 64;
constexpr unsigned long long kFanRefreshMs  = 2000;
constexpr unsigned long long kNodeRefreshMs = 2000;

// Both opt-in. They are debugging aids that cost calls into the game every
// second, and the agent has no use for them.
bool g_show_fan   = false;
bool g_show_nodes = false;
// What the last fan cost, so a stutter has a number next to it.
std::atomic<unsigned long long> g_fan_ms{0};
std::atomic<int>                g_fan_calls{0};

const ImU32 kLineOk      = IM_COL32(80, 220, 80, 230);
const ImU32 kLineBlocked = IM_COL32(240, 70, 70, 230);
const ImU32 kLineUnsure  = IM_COL32(240, 200, 60, 230);
const ImU32 kDotNode     = IM_COL32(200, 200, 200, 140);
const ImU32 kDotTarget   = IM_COL32(255, 255, 255, 255);
const ImU32 kTextShadow  = IM_COL32(0, 0, 0, 200);

// The fan: sixteen short walks from where the character stands, each ending
// where the world stopped it. Posted, because a few hundred calls into the
// game do not belong in a draw call.
void RefreshFan() {
  Bridge::PostToGameThread([]() {
    const samp::LocalPed self = samp::ReadLocalPed();
    if (!self.valid || !game::CallsTrusted()) return;
    const unsigned long long started = GetTickCount64();
    std::vector<game::Vec3> ends;
    std::vector<bool>       ok;
    const game::Vec3 here{self.x, self.y, self.z};
    int calls = 0;
    for (int i = 0; i < kFanSpokes; ++i) {
      const float angle = static_cast<float>(i) * 6.2831853f / kFanSpokes;
      const game::Vec3 to{self.x + std::cos(angle) * kFanMetres,
                          self.y + std::sin(angle) * kFanMetres, self.z};
      const nav::Verdict verdict = nav::Walkable(here, to);
      ends.push_back(verdict.where);
      ok.push_back(verdict.ok);
      calls += verdict.calls;
    }
    nav::SetDebugFan(std::move(ends), std::move(ok));
    g_fan_ms.store(GetTickCount64() - started);
    g_fan_calls.store(calls);
    static bool reported = false;
    if (!reported) {
      reported = true;
      LOG_INFO("reach fan: {} calls into the game in {} ms", calls,
               g_fan_ms.load());
    }
  });
}

void RefreshNodes() {
  Bridge::PostToGameThread([]() {
    const samp::LocalPed self = samp::ReadLocalPed();
    if (!self.valid) return;
    nav::SetDebugNodes(game::PedNodesNear(game::Vec3{self.x, self.y, self.z},
                                          kNodeRadius, kMaxDrawnNodes));
  });
}

void PlanAhead(float metres) {
  Bridge::PostToGameThread([metres]() {
    const samp::LocalPed self = samp::ReadLocalPed();
    if (!self.valid || !game::CallsTrusted()) return;
    const game::Vec3 here{self.x, self.y, self.z};
    const game::Vec3 target{self.x + std::cos(self.heading) * metres,
                            self.y + std::sin(self.heading) * metres, self.z};
    nav::SetDebugPlan(target, nav::PlanPath(here, target));
  });
}

void Shadowed(ImDrawList* draw, ImVec2 at, ImU32 colour, const char* text) {
  draw->AddText(ImVec2(at.x + 1, at.y + 1), kTextShadow, text);
  draw->AddText(at, colour, text);
}

// Everything the planner knows, drawn where it is: the route on the ground
// it runs over, the fan around the character's feet, the nodes on the
// pavements. This is the debug tool - a number in a panel says a leg is
// blocked; a red line on the screen says by what.
void DrawWorld(bool everything) {
  if (!game::CallsTrusted()) return;

  // Nothing asked for means nothing drawn, and nothing drawn means the game
  // is not asked anything. This used to project the player's own feet on
  // every frame whether or not there was a line to draw - ninety calls a
  // second against a ceiling of a hundred and twenty, so the ceiling was
  // reached by drawing alone and every other caller was told "no answer".
  const nav::DebugState debug = nav::GetDebug();
  const act::Status walk = act::Get();
  if (!everything && !debug.has_target) return;
  if (!g_show_fan && !g_show_nodes && !debug.has_target && !walk.walking &&
      debug.obstacles.empty())
    return;

  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) return;

  ImDrawList* draw = ImGui::GetBackgroundDrawList();

  float px = 0, py = 0;
  const bool self_on_screen =
      game::ToScreen(game::Vec3{self.x, self.y, self.z - 0.9f}, &px, &py);

  if (everything && g_show_nodes) {
    for (const game::PathNode& node : debug.nodes) {
      float sx = 0, sy = 0;
      if (!game::ToScreen(node.pos, &sx, &sy)) continue;
      draw->AddCircleFilled(ImVec2(sx, sy), 3.0f, kDotNode);
    }
  }

  // The fan: asked for, or the one the journey felt its way with.
  if (everything && (g_show_fan || walk.walking) && self_on_screen) {
    for (std::size_t i = 0; i < debug.fan_ends.size() && i < debug.fan_ok.size();
         ++i) {
      float sx = 0, sy = 0;
      const game::Vec3 foot{debug.fan_ends[i].x, debug.fan_ends[i].y,
                            debug.fan_ends[i].z - 0.9f};
      if (!game::ToScreen(foot, &sx, &sy)) continue;
      const ImU32 colour = debug.fan_ok[i] ? kLineOk : kLineBlocked;
      draw->AddLine(ImVec2(px, py), ImVec2(sx, sy), colour, 1.5f);
      draw->AddCircleFilled(ImVec2(sx, sy), 3.0f, colour);
    }
  }

  // The whiskers, while he walks: green where the way is clear, red where
  // it is not, from where they were cast.
  if (everything && walk.walking && !debug.whisker_ends.empty()) {
    float ox = 0, oy = 0;
    const game::Vec3 origin{debug.whisker_origin.x, debug.whisker_origin.y,
                            debug.whisker_origin.z - 0.9f};
    if (game::ToScreen(origin, &ox, &oy)) {
      for (std::size_t i = 0; i < debug.whisker_ends.size() &&
                              i < debug.whisker_clear.size(); ++i) {
        float sx = 0, sy = 0;
        const game::Vec3 end{debug.whisker_ends[i].x, debug.whisker_ends[i].y,
                             debug.whisker_ends[i].z - 0.9f};
        if (!game::ToScreen(end, &sx, &sy)) continue;
        const ImU32 colour = debug.whisker_clear[i] ? kLineOk : kLineBlocked;
        draw->AddLine(ImVec2(ox, oy), ImVec2(sx, sy), colour, 2.0f);
      }
    }
  }
  // What the walker met and the planner now goes round.
  if (everything) for (const game::Vec3& obstacle : debug.obstacles) {
    float sx = 0, sy = 0;
    if (!game::ToScreen(game::Vec3{obstacle.x, obstacle.y, obstacle.z - 0.5f},
                        &sx, &sy))
      continue;
    draw->AddCircle(ImVec2(sx, sy), 7.0f, kLineBlocked, 12, 2.0f);
    draw->AddLine(ImVec2(sx - 5, sy - 5), ImVec2(sx + 5, sy + 5), kLineBlocked, 2.0f);
    draw->AddLine(ImVec2(sx - 5, sy + 5), ImVec2(sx + 5, sy - 5), kLineBlocked, 2.0f);
  }

  if (debug.has_target) {
    const nav::Plan& plan = debug.plan;
    for (const nav::Leg& leg : plan.legs) {
      float ax = 0, ay = 0, bx = 0, by = 0;
      const game::Vec3 a{leg.from.x, leg.from.y, leg.from.z - 0.9f};
      const game::Vec3 b{leg.to.x, leg.to.y, leg.to.z - 0.9f};
      if (!game::ToScreen(a, &ax, &ay) || !game::ToScreen(b, &bx, &by)) continue;
      const ImU32 colour = !leg.verified ? kLineUnsure
                           : leg.ok      ? kLineOk
                                         : kLineBlocked;
      draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), colour, 3.0f);
      draw->AddCircle(ImVec2(bx, by), 5.0f, colour, 12, 2.0f);
      if (everything && !leg.ok && !leg.why.empty())
        Shadowed(draw, ImVec2(bx + 8, by - 8), kLineBlocked, leg.why.c_str());
    }
    float tx = 0, ty = 0;
    const game::Vec3 target{debug.target.x, debug.target.y, debug.target.z - 0.9f};
    if (game::ToScreen(target, &tx, &ty)) {
      draw->AddCircle(ImVec2(tx, ty), 9.0f, kDotTarget, 16, 2.0f);
      char label[96];
      const float dx = debug.target.x - self.x;
      const float dy = debug.target.y - self.y;
      if (everything)
        std::snprintf(label, sizeof(label), "%.1f m  %s",
                      std::sqrt(dx * dx + dy * dy),
                      plan.ok ? "ok" : plan.note.c_str());
      else
        std::snprintf(label, sizeof(label), "%.0f м", std::sqrt(dx * dx + dy * dy));
      Shadowed(draw, ImVec2(tx + 12, ty - 6), kDotTarget, label);
    }
  }
}

void DrawPanel() {
  static json               cached;
  static json               cached_world;
  static json               cached_chat;
  static unsigned long long cached_at = 0;
  static std::int64_t       cached_age = -1;
  const unsigned long long now = GetTickCount64();
  if (cached.is_null() || now - cached_at >= kRefreshMs) {
    cached = BuildStatusSnapshot();
    // The summary, not the world: the snapshot carries six hundred players
    // and a copy of it four times a second to show three numbers is waste.
    cached_world = Bridge::GetWorldSummary(&cached_age);
    // Cached for the same reason: reading the ring means validating an
    // address per entry, and doing that ninety times a second for six lines
    // a person is reading buys nothing.
    cached_chat = samp::CachedChat().valid ? samp::ReadChat(kChatLines)
                                           : json::object();
    cached_at = now;
  }

  const json& status = cached;
  const json  frame  = status.value("frame", json::object());
  const std::string verdict = status.value("verdict", std::string{"?"});

  // FirstUseEver, not Always: past the first run the position comes from
  // bot.imgui.ini, which is the point of being able to drag it.
  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(820, 400), ImGuiCond_FirstUseEver);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
  if (g_mode != Mode::kDevInteractive)
    flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoInputs;
  ImGui::Begin("gtabot", nullptr, flags);

  // Left: what the bot can see. Right: what it has been saying. Everything
  // that was here to prove the plumbing works - frame counters, patch bytes,
  // queue depths, pool addresses - has gone: it is answered by the verdict
  // when it matters and is noise when it does not.
  ImGui::BeginChild("left", ImVec2(kLeftColumn, 0), false);

  if (g_mode == Mode::kDevInteractive) {
    if (CursorHook::installed())
      ImGui::TextColored(kAmber, "interactive - the mouse is ours, F11 to "
                                 "give it back");
    else
      ImGui::TextColored(kRed, "interactive, but the mouse could not be taken "
                               "from the game");
  } else {
    ImGui::TextColored(kGrey, "passive - F11 closes; the menu's last item opens "
                              "this panel with the mouse");
  }

  const bool healthy = verdict == "ok";
  if (healthy) {
    char headline[64];
    std::snprintf(headline, sizeof(headline), "ok  -  %d fps",
                  static_cast<int>(frame.value("fps", 0.0)));
    Label("bot", headline, kGreen);
  } else {
    Label("bot", verdict, kAmber);
  }

  const samp::Client client = samp::Detect();
  const samp::Layout& layout = samp::ResolveLayout();
  if (layout.valid) {
    Label("server", layout.host + "   " + samp::ToString(client.version),
          kGreen);
  } else {
    Label("server", layout.note.empty() ? "not in a server" : layout.note,
          kAmber);
  }

  if (cached_world.value("resolved", false)) {
    const json world_layout = cached_world.value("layout", json::object());
    char counts[96];
    std::snprintf(counts, sizeof(counts), "%d players, %d near, %d cars",
                  cached_world.value("player_count", 0),
                  world_layout.value("players_streamed", 0),
                  cached_world.value("vehicle_count", 0));
    Label("world", counts);

    const json self = cached_world.value("self", json::object());
    if (!self.empty()) {
      char who[128];
      std::snprintf(who, sizeof(who), "id %d   hp %d   armour %d   %s",
                    self.value("id", -1),
                    static_cast<int>(self.value("health", 0.0f)),
                    static_cast<int>(self.value("armour", 0.0f)),
                    self.value("weapon_name", std::string{"?"}).c_str());
      Label("self", who);
      const json pos = self.value("pos", json::array());
      if (pos.size() == 3) {
        char where[64];
        std::snprintf(where, sizeof(where), "%.0f, %.0f, %.0f",
                      pos[0].get<float>(), pos[1].get<float>(),
                      pos[2].get<float>());
        Label("at", where);
      }
    }
  } else {
    Label("world", cached_age < 0 ? "never built" : "not resolved", kAmber);
  }

  const StatusSource::Mcp mcp = StatusSource::mcp();
  Label("mcp", mcp.listening ? mcp.endpoint : "not listening",
        mcp.listening ? kGreen : kRed);

  ImGui::Separator();
  // Movement: whether the calls into the game are trusted, what the graph
  // looks like from here, and what the last plan made of the world.
  {
    const game::Exe& exe = game::Detect();
    if (!exe.known) {
      Label("game", "not the 1.0 US build - no calls into it", kRed);
    } else if (!game::Enabled()) {
      Label("game", "movement OFF - tick to arm the game calls", kGrey);
    } else if (!game::CallsTrusted()) {
      Label("game", "armed, verifying (on foot, on the ground?)", kAmber);
    } else if (!game::LineOfSightTrusted()) {
      Label("game", "armed - ground only, line of sight not verified", kAmber);
    } else {
      Label("game", "armed and verified", kGreen);
    }
    if (g_mode == Mode::kDevInteractive && exe.known) {
      static bool enable = false;
      enable = game::Enabled();
      if (ImGui::Checkbox("enable movement (calls into the game)", &enable))
        game::SetEnabled(enable);
    }

    const game::PathLayout& graph = game::CachedPaths();
    if (graph.valid) {
      char text[96];
      std::snprintf(text, sizeof(text), "%d areas, %d ped nodes, nearest %.1f m",
                    graph.loaded_areas, graph.ped_nodes_loaded,
                    graph.nearest_ped_node_m);
      Label("graph", text, graph.nearest_ped_node_m >= 0 &&
                                   graph.nearest_ped_node_m < 40.0f
                               ? kGreen
                               : kAmber);
    } else {
      Label("graph", graph.note.empty() ? "not resolved" : graph.note, kAmber);
    }

    const nav::DebugState debug = nav::GetDebug();
    if (debug.has_target) {
      char text[160];
      std::snprintf(text, sizeof(text), "%s - %s, %.1f m, %d calls, %d ms",
                    debug.plan.ok ? "ok" : "NO", debug.plan.note.c_str(),
                    debug.plan.length_m, debug.plan.game_calls,
                    debug.plan.took_ms);
      Label("plan", text, debug.plan.ok ? kGreen : kAmber);
    } else {
      Label("plan", "none - ask for one below or via plan_path", kGrey);
    }

    // Who has the input right now. If the character will not move, these
    // two lines are the first thing to read: the second is the game's own
    // switch, the one SA-MP throws for a dialog and the server throws to
    // freeze a player - and a character held by that is not held by us.
    {
      char text[128];
      const bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
      const bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
      const bool s_ = (GetAsyncKeyState('S') & 0x8000) != 0;
      const bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
      std::snprintf(text, sizeof(text), "%s, mouse %s, keys to the game%s%s%s%s%s",
                    g_mode == Mode::kDevInteractive ? "interactive" : "passive",
                    g_mode == Mode::kDevInteractive ? "ours" : "the game's",
                    (w || a || s_ || d) ? "  held:" : "", w ? " W" : "",
                    a ? " A" : "", s_ ? " S" : "", d ? " D" : "");
      Label("input", text, g_mode == Mode::kDevInteractive ? kAmber : kGrey);

      bool disabled = false;
      if (game::ControlsDisabled(&disabled)) {
        Label("controls", disabled ? "DISABLED by the game (dialog, or the "
                                     "server froze the player)"
                                   : "enabled by the game",
              disabled ? kRed : kGrey);
        static bool last = false;
        static bool known = false;
        if (!known || disabled != last) {
          known = true;
          last  = disabled;
          LOG_INFO("the game reports player controls {}",
                   disabled ? "DISABLED" : "enabled");
        }
      }
      if (game::Enabled()) {
        std::snprintf(text, sizeof(text), "%d of %d a second",
                      game::CallsInLastSecond(), game::CallsPerSecondCeiling());
        Label("calls", text,
              game::CallsInLastSecond() >= game::CallsPerSecondCeiling()
                  ? kAmber
                  : kGrey);
      }
      if (g_show_fan) {
        std::snprintf(text, sizeof(text), "%d calls, %llu ms per refresh",
                      g_fan_calls.load(),
                      static_cast<unsigned long long>(g_fan_ms.load()));
        Label("fan", text, g_fan_ms.load() > 30 ? kAmber : kGrey);
      }
    }

    {
      const act::TravelStatus trip = act::TravelGet();
      if (trip.travelling) {
        char text[192];
        std::snprintf(text, sizeof(text),
                      "%.0f m to go, %d legs planned, %s", trip.straight_m,
                      trip.replans,
                      trip.reaching ? "heading for the far side" : "on the "
                                                                   "last leg");
        Label("journey", text, kGreen);
      } else if (!trip.note.empty() && trip.note != "idle") {
        Label("journey", trip.note,
              trip.note == "arrived" ? kGreen : kAmber);
      }

      const act::Status walk = act::Get();
      if (walk.walking) {
        char text[160];
        char round[48] = "";
        if (walk.sidesteps > 0)
          std::snprintf(round, sizeof(round), ", stepped round %d",
                        walk.sidesteps);
        std::snprintf(text, sizeof(text),
                      "leg %d of %d, %.1f m to it, %.1f m left%s%s%s%s",
                      walk.leg + 1, walk.legs, walk.to_next_m, walk.remaining_m,
                      round, walk.sprinting ? ", running" : "",
                      walk.wall ? ", WALL AHEAD" : "",
                      std::fabs(walk.steer_deg) > 1.0f ? ", leaning" : "");
        Label("walking", text, walk.wall ? kAmber : kGreen);
        if (walk.jumps > 0 || std::fabs(walk.steer_deg) > 1.0f) {
          std::snprintf(text, sizeof(text), "%d jumps, leaning %.0f deg",
                        walk.jumps, walk.steer_deg);
          Label("", text, kGrey);
        }
      } else {
        Label("walking", walk.note.empty() ? "idle" : walk.note, kGrey);
      }
      if (walk.corrected) {
        char text[96];
        std::snprintf(text, sizeof(text), "sideways axis corrected (%.0f deg out)",
                      walk.error_deg);
        Label("steering", text, kAmber);
      }
    }

    if (g_mode == Mode::kDevInteractive) {
      // A destination to type, because crossing a city is the thing worth
      // testing and "fifteen metres ahead" cannot test it.
      static float target[2] = {0, 0};
      static bool  target_set = false;
      const samp::LocalPed self = samp::ReadLocalPed();
      if (!target_set && self.valid) {
        target[0] = self.x;
        target[1] = self.y;
        target_set = true;
      }
      ImGui::SetNextItemWidth(180.0f);
      ImGui::InputFloat2("##target", target, "%.0f");
      ImGui::SameLine();
      if (ImGui::Button("Travel there")) {
        const float dx = target[0] - (self.valid ? self.x : 0.0f);
        const float dy = target[1] - (self.valid ? self.y : 0.0f);
        if (std::sqrt(dx * dx + dy * dy) < 5.0f)
          SetReportSummary("that is where he already is - type somewhere else");
        else
          act::TravelTo(game::Vec3{target[0], target[1],
                                   self.valid ? self.z : 0.0f});
      }
      ImGui::SameLine();
      if (ImGui::Button("Here")) {
        if (self.valid) {
          target[0] = self.x;
          target[1] = self.y;
        }
      }
      if (ImGui::Button("Travel 250 m away")) {
        // The furthest loaded ped node, which is a real errand across
        // streets rather than a straight line down one.
        if (self.valid) {
          const game::Vec3 here{self.x, self.y, self.z};
          const std::vector<game::PathNode> nodes =
              game::PedNodesNear(here, 250.0f, 600);
          const game::PathNode* furthest = nullptr;
          float best = 0;
          for (const game::PathNode& node : nodes) {
            const float dx = node.pos.x - here.x, dy = node.pos.y - here.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d <= best) continue;
            best = d;
            furthest = &node;
          }
          if (furthest != nullptr) {
            target[0] = furthest->pos.x;
            target[1] = furthest->pos.y;
            act::TravelTo(game::Vec3{furthest->pos.x, furthest->pos.y,
                                     furthest->pos.z + 1.0f});
          } else {
            SetReportSummary("no ped nodes loaded to aim at");
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel journey")) act::CancelTravel("cancelled");

      if (ImGui::Button("Walk the plan")) {
        // Saying why, rather than doing nothing: a button that silently
        // ignores a press is indistinguishable from one that is broken.
        const nav::DebugState debug = nav::GetDebug();
        if (!debug.has_target)
          SetReportSummary("no plan to walk - ask for one first");
        else if (!debug.plan.ok)
          SetReportSummary("that plan is not walkable: " + debug.plan.note);
        else if (debug.plan.waypoints.size() < 2)
          SetReportSummary("that plan has nowhere to go");
        else
          act::WalkTo(std::vector<game::Vec3>(debug.plan.waypoints.begin() + 1,
                                              debug.plan.waypoints.end()));
      }
      ImGui::SameLine();
      if (ImGui::Button("Stop walking")) act::Stop("stopped from the panel");
      if (ImGui::Button("Plan 15 m ahead")) PlanAhead(15.0f);
      ImGui::SameLine();
      if (ImGui::Button("Plan 60 m ahead")) PlanAhead(60.0f);
      ImGui::SameLine();
      if (ImGui::Button("Clear")) nav::ClearDebug();
      ImGui::Checkbox("fan", &g_show_fan);
      ImGui::SameLine();
      ImGui::Checkbox("nodes", &g_show_nodes);
      ImGui::SameLine();
      bool sprint = act::Sprint();
      if (ImGui::Checkbox("sprint", &sprint)) act::SetSprint(sprint);
      ImGui::SameLine();
      bool hop = act::BunnyHop();
      if (ImGui::Checkbox("bunny hop", &hop)) act::SetBunnyHop(hop);
      ImGui::SameLine();
      if (ImGui::Button("Forget obstacles")) nav::ForgetObstacles();
      bool ignore_pad = game::IgnoreGamepad();
      if (ImGui::Checkbox("ignore gamepad (it can switch WASD off)", &ignore_pad))
        game::SetIgnoreGamepad(ignore_pad);
      ImGui::SameLine();
      bool kb_fallback = game::KeyboardFallback();
      if (ImGui::Checkbox("keyboard fallback", &kb_fallback))
        game::SetKeyboardFallback(kb_fallback);
    }

    // The fan and the nodes follow the character, on their own clocks.
    static unsigned long long fan_at = 0, nodes_at = 0;
    if (g_show_fan && now - fan_at >= kFanRefreshMs) {
      fan_at = now;
      RefreshFan();
    }
    if (g_show_nodes && now - nodes_at >= kNodeRefreshMs) {
      nodes_at = now;
      RefreshNodes();
    }
  }

  ImGui::Separator();
  // The chat, drawn next to the real one on purpose: the only way to know a
  // column was labelled right is to read it against what is on screen.
  const samp::ChatLayout& chat = samp::CachedChat();
  if (chat.valid) {
    char shape[128];
    // The count the reader actually produced, not the number of slots that
    // held bytes: the two differ by whatever the signature threw out.
    std::snprintf(shape, sizeof(shape), "%d lines, %s, %s",
                  cached_chat.value("count", 0),
                  chat.anchored ? "anchored" : "by shape only",
                  chat.order_known
                      ? (chat.newest_first ? "newest first" : "oldest first")
                      : "no clock column");
    Label("chat", shape, chat.anchored ? kGreen : kAmber);
    for (const json& line : cached_chat.value("lines", json::array())) {
      const std::string from = line.value("from", std::string{});
      ImGui::TextWrapped("  %s%s", from.empty() ? "" : (from + "  ").c_str(),
                         line.value("text", std::string{}).c_str());
    }
  } else {
    Label("chat", chat.note.empty() ? "not resolved" : chat.note, kAmber);
  }

  if (g_mode == Mode::kDevInteractive) {
    ImGui::Separator();
    if (ImGui::Button("Dump chat"))
      Bridge::PostToGameThread([]() { samp::DumpChat(); });
    ImGui::SameLine();
    if (ImGui::Button("Dump structures")) {
      SetReportSummary("running...");
      Bridge::PostToGameThread([]() {
        samp::DumpPlayerRecords();
        const samp::ReportOutcome outcome = samp::WriteStructureReport();
        SetReportSummary(outcome.written ? "wrote " + outcome.path
                                         : outcome.error);
      });
    }
    const std::string report_summary = ReportSummary();
    if (!report_summary.empty())
      ImGui::TextWrapped("%s", report_summary.c_str());
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("log", ImVec2(0, 0), true,
                    ImGuiWindowFlags_NoNav |
                        (g_mode == Mode::kDevInteractive
                             ? 0
                             : ImGuiWindowFlags_NoInputs));
  for (const LogLine& line : RecentLogLines(kLogLines)) {
    ImGui::PushStyleColor(ImGuiCol_Text, LevelColour(line.level));
    ImGui::TextWrapped("%s", line.text.c_str());
    ImGui::PopStyleColor();
  }
  // Follow the tail, unless the reader has scrolled up to look at something.
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
    ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();

  ImGui::End();
}

// ---- drawing the player's interface -----------------------------------------

void Txt(ImDrawList* draw, ImFont* font, float px, ImVec2 at, ImU32 colour,
         const char* text, float wrap = 0.0f) {
  draw->AddText(font, px, at, colour, text, nullptr, wrap);
}

float Wid(ImFont* font, float px, const char* text) {
  return font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
}

struct Line {
  std::string text;
  ImU32       colour = kUiText;
};

// What the player's interface needs to know, refreshed a few times a second
// rather than every frame: the status json and the world summary cost
// allocations, and a person reads none of it faster than that.
struct PlayerView {
  bool        in_game = false;
  std::string server;
  bool        self_known = false;
  int         hp = 0, armour = 0;
  std::string verdict;
  bool        marker = false;
  float       marker_m = 0;
  game::Vec3  marker_pos;
  unsigned long long at = 0;
};

const PlayerView& RefreshPlayerView(unsigned long long now) {
  static PlayerView view;
  if (view.at != 0 && now - view.at < kRefreshMs) return view;
  view.at = now;
  const json status = BuildStatusSnapshot();
  view.verdict = status.value("verdict", std::string{"?"});
  const samp::Layout& layout = samp::ResolveLayout();
  view.in_game = layout.valid;
  view.server  = layout.valid ? layout.host : std::string{};
  std::int64_t age = -1;
  const json world = Bridge::GetWorldSummary(&age);
  const json self  = world.value("self", json::object());
  view.self_known = world.value("resolved", false) && !self.empty();
  if (view.self_known) {
    view.hp     = static_cast<int>(self.value("health", 0.0f));
    view.armour = static_cast<int>(self.value("armour", 0.0f));
  }
  view.marker = game::MapMarker(&view.marker_pos);
  if (view.marker) {
    const samp::LocalPed ped = samp::ReadLocalPed();
    if (ped.valid) {
      const float dx = view.marker_pos.x - ped.x, dy = view.marker_pos.y - ped.y;
      view.marker_m = std::sqrt(dx * dx + dy * dy);
    }
  }
  return view;
}

// The game translates a letter key through the current keyboard layout
// (MapVirtualKeyA with MAPVK_VK_TO_CHAR) and files it under the character it
// gets, so with a Cyrillic layout W arrives as "ц" and nothing bound to W
// sees it. Every player learns this the hard way once; the panel says it.
bool CyrillicLayout() {
  if (g_window == nullptr) return false;
  const DWORD tid = GetWindowThreadProcessId(g_window, nullptr);
  const auto layout = static_cast<unsigned>(
      reinterpret_cast<std::uintptr_t>(GetKeyboardLayout(tid)) & 0xFFFF);
  return layout == 0x0419 || layout == 0x0422 || layout == 0x0423 ||
         layout == 0x043F;
}

// And fixes it. The game asks Windows what character a key produces and
// files the key under that character, so under a Cyrillic layout W arrives
// as "ц", T does not open the chat, and nothing bound to a letter works at
// all. Asking the window to switch to English is what a player does without
// thinking, it affects this window and nothing else, and it is the
// difference between a character who can talk and one who cannot.
void AskForALatinLayout() {
  if (g_window == nullptr) return;
  static unsigned long long asked_ms = 0;
  const unsigned long long now = GetTickCount64();
  if (now - asked_ms < 5000) return;
  asked_ms = now;
  const HKL english = LoadKeyboardLayoutW(L"00000409", KLF_ACTIVATE);
  if (english == nullptr) return;
  PostMessageW(g_window, WM_INPUTLANGCHANGEREQUEST, INPUTLANGCHANGE_SYSCHARSET,
               reinterpret_cast<LPARAM>(english));
  static bool said = false;
  if (!said) {
    said = true;
    LOG_INFO("layout: the window is on a Cyrillic layout, where the game sees "
             "no letter keys at all - asking it for English");
  }
}

// The bot, in one word.
Line BotState(const PlayerView& view) {
  if (!game::Detect().known) return {"игра не поддерживается", kUiBad};
  if (!view.in_game) return {"не в игре", kUiMuted};
  if (CyrillicLayout()) {
    AskForALatinLayout();
    return {"раскладка RU: переключаю на EN", kUiWarn};
  }
  if (view.verdict != "ok") return {"ожидание игры", kUiWarn};
  if (!game::Enabled()) return {"управление выключено", kUiDim};
  if (!game::CallsTrusted()) return {"проверка…", kUiWarn};
  return {"готов", kUiOk};
}

// The journey, in a few words, and how much of it is done (below zero when
// there is nothing to measure).
Line JourneyState(float* progress) {
  const act::TravelStatus trip = act::TravelGet();
  const act::Status walk = act::Get();
  static float start_m = 0;
  static bool  was = false;
  if (trip.travelling && !was) start_m = trip.straight_m;
  was = trip.travelling;
  *progress = -1.0f;
  char text[96];
  if (trip.travelling) {
    if (start_m > 1.0f)
      *progress = std::clamp(1.0f - trip.straight_m / start_m, 0.0f, 1.0f);
    std::snprintf(text, sizeof(text), "%s · %.0f м",
                  walk.wall ? "обходит препятствие"
                  : walk.sprinting ? "бежит" : "идёт",
                  trip.straight_m);
    return {text, kUiOk};
  }
  if (walk.walking) {
    std::snprintf(text, sizeof(text), "идёт · %.0f м", walk.remaining_m);
    return {text, kUiOk};
  }
  if (trip.note == "arrived") return {"пришёл на место", kUiOk};
  if (trip.note.rfind("gave up", 0) == 0) return {"не смог дойти", kUiWarn};
  if (trip.note.find("stopped") != std::string::npos ||
      trip.note.find("cancelled") != std::string::npos)
    return {"остановлен", kUiMuted};
  return {"нет", kUiDim};
}

// What "go to the marker" does: arms the game calls if they are not, and
// sets off. Says why when it cannot.
void GoToMarker(const PlayerView& view) {
  if (!game::Detect().known) {
    Say("Эта версия игры не поддерживается");
    return;
  }
  game::Vec3 marker;
  if (!game::MapMarker(&marker)) {
    Say("Сначала поставьте метку на карте: Esc → Карта → клик");
    return;
  }
  if (!view.in_game) {
    Say("Персонаж не в игре");
    return;
  }
  if (!game::Enabled()) game::SetEnabled(true);
  act::TravelTo(marker, /*height_unknown=*/true);
  char text[64];
  std::snprintf(text, sizeof(text), "Иду к метке, %.0f м", view.marker_m);
  Say(text);
  LOG_INFO("menu: go to the map marker at ({:.0f}, {:.0f})", marker.x, marker.y);
}

void StopEverything() {
  act::CancelTravel("stopped from the menu");
  act::Stop("stopped from the menu");
}

enum Item {
  kItemGo, kItemStop, kItemControl, kItemSprint, kItemHop, kItemRoute,
  kItemHud, kItemKeyTest, kItemWorldTest, kItemDeveloper, kItemCount
};

struct Row {
  const char* label;
  std::string value;
  ImU32       value_colour = kUiText;
  bool        enabled = true;
  bool        toggle  = false;
  bool        on      = false;
};

// direction: 0 for Enter, -1 for left, +1 for right.
void Activate(int item, int direction, const PlayerView& view) {
  const auto want = [&](bool current) { return direction == 0 ? !current : direction > 0; };
  switch (item) {
    case kItemGo:
      if (direction == 0) GoToMarker(view);
      break;
    case kItemStop:
      if (direction == 0) {
        StopEverything();
        Say("Остановился");
      }
      break;
    case kItemControl: {
      const bool on = want(game::Enabled());
      if (!on) StopEverything();
      game::SetEnabled(on);
      break;
    }
    case kItemSprint:  act::SetSprint(want(act::Sprint()));     break;
    case kItemHop:     act::SetBunnyHop(want(act::BunnyHop()));  break;
    case kItemRoute:   g_show_route = want(g_show_route);        break;
    case kItemHud:     g_hud = want(g_hud);                      break;
    case kItemKeyTest:
      if (direction != 0) break;
      if (act::ExperimentRunning()) {
        act::StopExperiments();
        Say("Опыт остановлен");
      } else {
        act::StartKeyRun(30);
        Say("30 с бега клавишами, без вызовов в игру. Смотри лог.");
      }
      break;
    case kItemWorldTest:
      if (direction != 0) break;
      if (act::ExperimentRunning()) {
        act::StopExperiments();
        Say("Опыт остановлен");
      } else {
        act::StartWorldCalls(30);
        Say("30 с чтения мира, без клавиш и без вызовов в игру. Смотри лог.");
      }
      break;
    case kItemDeveloper:
      if (direction == 0) SetMode(Mode::kDevInteractive);
      break;
    default:
      break;
  }
}

void DrawRow(ImDrawList* draw, ImVec2 at, float width, const Row& row,
             bool selected) {
  const float s = g_scale;
  const float h = kRowHeight * s;
  if (selected) {
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + h), kUiAccentDim, 6 * s);
    draw->AddRectFilled(at, ImVec2(at.x + 3 * s, at.y + h), kUiAccent, 2 * s);
  }
  const float ty = at.y + (h - kBodyPx * s) * 0.5f;
  Txt(draw, g_body, kBodyPx * s, ImVec2(at.x + 14 * s, ty),
      row.enabled ? kUiText : kUiDim, row.label);
  if (row.toggle) {
    const char* word = row.on ? "вкл" : "выкл";
    const float tw = Wid(g_body, kSmallPx * s, word);
    const float pw = tw + 18 * s, ph = 20 * s;
    const ImVec2 p0(at.x + width - 12 * s - pw, at.y + (h - ph) * 0.5f);
    draw->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph),
                        row.on ? kUiAccent : kUiPill, ph * 0.5f);
    Txt(draw, g_body, kSmallPx * s,
        ImVec2(p0.x + 9 * s, p0.y + (ph - kSmallPx * s) * 0.5f),
        row.on ? kUiPillText : kUiMuted, word);
  } else if (!row.value.empty()) {
    const float vw = Wid(g_body, kBodyPx * s, row.value.c_str());
    Txt(draw, g_body, kBodyPx * s, ImVec2(at.x + width - 12 * s - vw, ty),
        row.value_colour, row.value.c_str());
  }
}

void DrawInfo(ImDrawList* draw, ImVec2 at, float width, const char* label,
              const Line& value) {
  const float s = g_scale;
  Txt(draw, g_body, kBodyPx * s, at, kUiMuted, label);
  Txt(draw, g_body, kBodyPx * s, ImVec2(at.x + 108 * s, at.y), value.colour,
      value.text.c_str(), width - 108 * s);
}

void DrawMenu(unsigned long long now) {
  const float s = g_scale;
  const ImGuiIO& io = ImGui::GetIO();
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const PlayerView& view = RefreshPlayerView(now);
  const Line bot = BotState(view);
  float progress = -1.0f;
  const Line trip = JourneyState(&progress);
  const bool moving = act::TravelGet().travelling || act::Get().walking;
  const bool game_known = game::Detect().known;

  // Keys first, so what is drawn is what was chosen.
  if (Fired(now, VK_ESCAPE)) {
    SetMode(Mode::kClosed);
    return;
  }
  int move = 0;
  if (Fired(now, VK_UP, 'W')) move = -1;
  if (Fired(now, VK_DOWN, 'S')) move = 1;
  g_selected = (g_selected + move + kItemCount) % kItemCount;
  int direction = 0;
  bool activate = false;
  if (Fired(now, VK_LEFT, 'A')) { activate = true; direction = -1; }
  if (Fired(now, VK_RIGHT, 'D')) { activate = true; direction = 1; }
  if (Fired(now, VK_RETURN, VK_SPACE)) { activate = true; direction = 0; }
  if (activate) Activate(g_selected, direction, view);
  if (g_mode != Mode::kMenu) return;   // the developer's panel was chosen

  Row rows[kItemCount];
  char marker_text[32] = "нет метки";
  if (view.marker) std::snprintf(marker_text, sizeof(marker_text), "%.0f м", view.marker_m);
  rows[kItemGo]        = {"Идти к метке на карте", marker_text,
                          view.marker ? kUiText : kUiDim, game_known};
  rows[kItemStop]      = {"Остановиться", "", kUiText, moving};
  rows[kItemControl]   = {"Управление персонажем", "", kUiText, game_known, true,
                          game::Enabled()};
  rows[kItemSprint]    = {"Спринт", "", kUiText, true, true, act::Sprint()};
  rows[kItemHop]       = {"Прыжки на бегу", "", kUiText, true, true, act::BunnyHop()};
  rows[kItemRoute]     = {"Показывать маршрут", "", kUiText, true, true, g_show_route};
  rows[kItemHud]       = {"Индикатор в углу", "", kUiText, true, true, g_hud};
  const std::string experiment = act::ExperimentLine();
  rows[kItemKeyTest]   = {"Опыт: бег клавишами 30 с", experiment, kUiAccent, game_known};
  rows[kItemWorldTest] = {"Опыт: 30 с чтения мира", experiment, kUiAccent, game_known};
  rows[kItemDeveloper] = {"Панель разработчика", "›", kUiMuted};

  const bool message = now < g_message_until && !g_message.empty();
  const float width = kMenuWidth * s;
  const float pad   = kMenuPad * s;
  const float header = 30 * s;
  const float info   = 4 * kInfoRow * s + (progress >= 0 ? 10 * s : 0);
  const float height = pad + header + 10 * s + info + 12 * s + 1 + 10 * s +
                       kItemCount * kRowHeight * s + 10 * s + 1 + 10 * s +
                       kSmallPx * s + (message ? 2 * kSmallPx * s + 8 * s : 0) +
                       pad;
  const ImVec2 p0(io.DisplaySize.x - width - 28 * s, io.DisplaySize.y * kMenuTop);
  const ImVec2 p1(p0.x + width, p0.y + height);
  draw->AddRectFilled(p0, p1, kUiBg, 10 * s);
  draw->AddRect(p0, p1, kUiBorder, 10 * s);

  // Header: the dot, the name, the version, and the state at the right.
  float y = p0.y + pad;
  const float x = p0.x + pad;
  draw->AddCircleFilled(ImVec2(x + 6 * s, y + header * 0.5f), 5 * s, bot.colour);
  Txt(draw, g_bold, kTitlePx * s, ImVec2(x + 20 * s, y + (header - kTitlePx * s) * 0.5f),
      kUiText, "GTABOT");
  const float name_w = Wid(g_bold, kTitlePx * s, "GTABOT");
  Txt(draw, g_body, kSmallPx * s,
      ImVec2(x + 20 * s + name_w + 8 * s, y + (header - kSmallPx * s) * 0.5f + 2 * s),
      kUiDim, GTABOT_VERSION);
  const float state_w = Wid(g_body, kBodyPx * s, bot.text.c_str());
  Txt(draw, g_body, kBodyPx * s,
      ImVec2(p1.x - pad - state_w, y + (header - kBodyPx * s) * 0.5f), bot.colour,
      bot.text.c_str());
  y += header + 10 * s;

  // What is going on.
  char self_text[64] = "—";
  if (view.self_known)
    std::snprintf(self_text, sizeof(self_text), "здоровье %d · броня %d", view.hp,
                  view.armour);
  const StatusSource::Mcp mcp = StatusSource::mcp();
  char agent_text[128];
  if (mcp.listening)
    std::snprintf(agent_text, sizeof(agent_text), "%s · %llu запросов",
                  mcp.endpoint.c_str(), static_cast<unsigned long long>(mcp.requests));
  else
    std::snprintf(agent_text, sizeof(agent_text), "не запущен");
  const float info_w = width - 2 * pad;
  DrawInfo(draw, ImVec2(x, y), info_w, "Сервер",
           {view.in_game ? view.server : "не подключён", view.in_game ? kUiText : kUiMuted});
  y += kInfoRow * s;
  DrawInfo(draw, ImVec2(x, y), info_w, "Персонаж", {self_text, kUiText});
  y += kInfoRow * s;
  DrawInfo(draw, ImVec2(x, y), info_w, "ИИ-агент",
           {agent_text, mcp.listening ? kUiText : kUiBad});
  y += kInfoRow * s;
  DrawInfo(draw, ImVec2(x, y), info_w, "Маршрут", trip);
  y += kInfoRow * s;
  if (progress >= 0) {
    const ImVec2 b0(x, y), b1(x + info_w, y + 4 * s);
    draw->AddRectFilled(b0, b1, kUiPill, 2 * s);
    draw->AddRectFilled(b0, ImVec2(b0.x + info_w * progress, b1.y), kUiAccent, 2 * s);
    y += 10 * s;
  }
  y += 12 * s;
  draw->AddLine(ImVec2(x, y), ImVec2(p1.x - pad, y), kUiLine);
  y += 1 + 10 * s;

  // The menu itself.
  for (int i = 0; i < kItemCount; ++i) {
    DrawRow(draw, ImVec2(x, y), info_w, rows[i], i == g_selected);
    y += kRowHeight * s;
  }
  y += 10 * s;
  draw->AddLine(ImVec2(x, y), ImVec2(p1.x - pad, y), kUiLine);
  y += 1 + 10 * s;

  // How to drive it, and the last thing it had to say.
  Txt(draw, g_body, kSmallPx * s, ImVec2(x, y), kUiDim,
      "↑↓ выбор  ·  Enter выбрать  ·  ←→ вкл/выкл  ·  Esc закрыть");
  y += kSmallPx * s;
  if (message) {
    y += 8 * s;
    Txt(draw, g_body, kSmallPx * s, ImVec2(x, y), kUiWarn, g_message.c_str(), info_w);
  }
}

// The badge in the corner while the menu is closed: the dot, the name, what
// he is doing, and the key that opens the menu.
// What the brain says it is doing, under the badge.
//
// Where the character went is visible; why is not, and without it a
// perfectly sensible plan looks like a man wandering about at random. So the
// brain posts a line of intent and its steps, and they are drawn here with
// the one under way marked. Nothing is checked: this is a caption, worth
// what the brain's honesty about itself is worth.
void DrawPlan(unsigned long long now, float below) {
  const state::Plan plan = state::GetPlan();
  // An empty panel is indistinguishable from a broken one, and the brain
  // going quiet is exactly what somebody watching needs to see. So it always
  // says something: what was last said, or that nothing has been.
  const bool nothing_said = plan.summary.empty();
  const bool gone_quiet = !nothing_said && plan.age_ms > 120000;
  const std::string title =
      nothing_said ? std::string("мозг ещё ничего не сказал")
                   : (gone_quiet ? plan.summary + "  (молчит)" : plan.summary);

  const float s = g_scale;
  const ImGuiIO& io = ImGui::GetIO();
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const float title_px = 13 * s, step_px = 12 * s;
  const float pad = 12 * s, gap = 6 * s;

  float width = Wid(g_bold, title_px, title.c_str());
  for (const std::string& step : plan.steps) {
    const float w = Wid(g_body, step_px, step.c_str()) + 18 * s;
    if (w > width) width = w;
  }
  const float box_w = width + pad * 2;
  const float box_h = pad + title_px + gap + 2 * (kSmallPx * s) + gap * 1.2f +
                      plan.steps.size() * (step_px + gap * 0.6f) + pad * 0.6f;
  const ImVec2 p0(io.DisplaySize.x - box_w - 28 * s, below + 8 * s);
  const ImVec2 p1(p0.x + box_w, p0.y + box_h);
  draw->AddRectFilled(p0, p1, kUiBg, 8 * s);
  draw->AddRect(p0, p1, kUiBorder, 8 * s);

  float y = p0.y + pad;
  Txt(draw, g_bold, title_px, ImVec2(p0.x + pad, y), nothing_said ? kUiDim : kUiText,
      title.c_str());
  y += title_px + gap;

  // When it last spoke, and how long it took to work this out. Both are the
  // module's own measurements: how stale the caption is, and the gap between
  // the brain asking what the world looked like and saying what it would do.
  char when[96];
  const long long seconds = plan.age_ms / 1000;
  if (nothing_said)
    std::snprintf(when, sizeof(when), "жду плана от ИИ");
  else if (plan.thought_ms >= 0)
    std::snprintf(when, sizeof(when), "%lld c назад  ·  думал %.1f c",
                  seconds, plan.thought_ms / 1000.0);
  else
    std::snprintf(when, sizeof(when), "%lld c назад", seconds);
  Txt(draw, g_body, kSmallPx * s, ImVec2(p0.x + pad, y), kUiDim, when);
  y += kSmallPx * s + gap * 0.4f;

  // And when it last asked what the world looks like. A brain still making up
  // its mind posts nothing, and this is the only thing that moves meanwhile.
  char asked[64];
  if (plan.looked_ago_ms >= 0)
    std::snprintf(asked, sizeof(asked), "спрашивал %llds назад",
                  plan.looked_ago_ms / 1000);
  else
    std::snprintf(asked, sizeof(asked), "ещё не спрашивал");
  Txt(draw, g_body, kSmallPx * s, ImVec2(p0.x + pad, y),
      plan.looked_ago_ms >= 0 && plan.looked_ago_ms < 15000 ? kUiOk : kUiDim,
      asked);
  y += kSmallPx * s + gap * 0.8f;
  for (std::size_t i = 0; i < plan.steps.size(); ++i) {
    const bool doing = static_cast<int>(i) == plan.doing;
    const ImU32 colour = doing ? kUiAccent : kUiDim;
    if (doing)
      draw->AddCircleFilled(ImVec2(p0.x + pad + 4 * s, y + step_px * 0.5f),
                            3 * s, kUiAccent);
    Txt(draw, g_body, step_px, ImVec2(p0.x + pad + 14 * s, y), colour,
        plan.steps[i].c_str());
    y += step_px + gap * 0.6f;
  }
  (void)now;
}

void DrawBadge(unsigned long long now) {
  const float s = g_scale;
  const ImGuiIO& io = ImGui::GetIO();
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const PlayerView& view = RefreshPlayerView(now);
  const Line bot = BotState(view);
  float progress = -1.0f;
  const Line trip = JourneyState(&progress);
  const bool moving = act::TravelGet().travelling || act::Get().walking;
  const Line& shown = moving ? trip : bot;

  const float h = 30 * s;
  const float name_px = 14 * s, text_px = 14 * s;
  const float name_w = Wid(g_bold, name_px, "GTABOT");
  const float text_w = Wid(g_body, text_px, shown.text.c_str());
  const float key_w  = Wid(g_body, kSmallPx * s, "F11");
  const float w = 14 * s + 10 * s + 8 * s + name_w + 10 * s + text_w + 14 * s + key_w + 14 * s;
  const ImVec2 p0(io.DisplaySize.x - w - 28 * s, io.DisplaySize.y * kMenuTop);
  const ImVec2 p1(p0.x + w, p0.y + h);
  draw->AddRectFilled(p0, p1, kUiBg, h * 0.5f);
  draw->AddRect(p0, p1, kUiBorder, h * 0.5f);

  float x = p0.x + 14 * s;
  draw->AddCircleFilled(ImVec2(x + 5 * s, p0.y + h * 0.5f), 5 * s,
                        moving ? kUiOk : bot.colour);
  x += 10 * s + 8 * s;
  Txt(draw, g_bold, name_px, ImVec2(x, p0.y + (h - name_px) * 0.5f), kUiText, "GTABOT");
  x += name_w + 10 * s;
  Txt(draw, g_body, text_px, ImVec2(x, p0.y + (h - text_px) * 0.5f), shown.colour,
      shown.text.c_str());
  Txt(draw, g_body, kSmallPx * s,
      ImVec2(p1.x - 14 * s - key_w, p0.y + (h - kSmallPx * s) * 0.5f), kUiDim, "F11");
  if (progress >= 0) {
    const float inset = 16 * s;
    const ImVec2 b0(p0.x + inset, p1.y - 5 * s), b1(p1.x - inset, p1.y - 3 * s);
    draw->AddRectFilled(b0, b1, kUiPill, 1 * s);
    draw->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * progress, b1.y), kUiAccent, 1 * s);
  }
  DrawPlan(now, p1.y);
}

}  // namespace

void Overlay::Render(IDirect3DDevice9* device) {
  if (!device) return;
  if (g_disabled) {
    // The key is still polled, so a way back exists.
    const bool down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
    if (down && !g_toggle_down) {
      g_disabled = false;
      SetMode(Mode::kClosed);
      LOG_INFO("overlay re-enabled");
    }
    g_toggle_down = down;
    return;
  }

  // Focus is the earliest warning that a reset is coming, and unlike the
  // device state it is readable before anything has gone wrong yet.
  ReleaseIfUnfocused();

  // A lost device cannot be drawn on, and a reset is imminent. Let go of
  // everything now rather than waiting to be told.
  if (device->TestCooperativeLevel() != D3D_OK) {
    OnLostDevice();
    return;
  }
  if (g_window && GetForegroundWindow() != g_window) return;

  // The game can recreate its device outright rather than resetting it, which
  // leaves the backend pointing at a dead object.
  if (g_initialised && device != g_device) {
    LOG_WARN("d3d9 device changed, rebuilding the overlay");
    Teardown();
  }
  if (!g_initialised && !Initialise(device)) return;

  TendWndProcChain(GetTickCount64());
  PollToggle();
  // Nothing of ours over the game's own menu - the map, where the marker is
  // put - and the keyboard menu closes there, so the keys are the game's.
  if (PauseMenuOpen()) {
    if (g_mode == Mode::kMenu) SetMode(Mode::kClosed);
    return;
  }
  const bool developer =
      g_mode == Mode::kDevPassive || g_mode == Mode::kDevInteractive;
  const unsigned long long now = GetTickCount64();
  const bool moving = act::TravelGet().travelling || act::Get().walking;
  const bool route  = g_show_route && moving;
  if (!developer && g_mode == Mode::kClosed && !g_hud && !route) return;

  // Only the back buffer. GTA ends a scene for every off-screen target it
  // renders - the radar, mirrors, the text baked onto signs - and drawing into
  // one of those bakes this panel into a game texture, which is exactly what
  // it looked like.
  if (!IsBackBufferBound(device)) return;

  g_resources_live = true;
  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  // The backend has just asked Windows where the cursor is and been told what
  // the game is told: the middle of the screen, every frame. The real position
  // comes through the untouched original, and the later event wins.
  if (g_mode == Mode::kDevInteractive && CursorHook::freed()) {
    POINT pointer{};
    if (CursorHook::RealCursorPos(&pointer) &&
        ScreenToClient(g_window, &pointer))
      ImGui::GetIO().AddMousePosEvent(static_cast<float>(pointer.x),
                                      static_cast<float>(pointer.y));
  }
  ImGui::NewFrame();
  if (developer) {
    DrawWorld(true);
    DrawPanel();
  } else {
    if (route) DrawWorld(false);
    if (g_mode == Mode::kMenu)
      DrawMenu(now);
    else if (g_hud)
      DrawBadge(now);
  }
  ImGui::EndFrame();
  ImGui::Render();
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void Overlay::OnLostDevice() {
  if (!g_initialised || !g_resources_live) return;
  ImGui_ImplDX9_InvalidateDeviceObjects();
  g_resources_live = false;
  LOG_INFO("released d3d9 resources ahead of a device reset");
}

void Overlay::OnResetDevice() {
  // Deliberately does not recreate anything here. ImGui_ImplDX9_NewFrame
  // rebuilds the font texture on its own once the device is usable again, and
  // doing it early - while the game may still be mid-reset - is how the
  // recreate ends up on a device that is not ready.
}

void Overlay::ReleaseIfUnfocused() {
  if (!g_initialised || !g_resources_live || !g_window) return;
  if (GetForegroundWindow() == g_window) return;
  OnLostDevice();
}

std::string Overlay::InputState() {
  char text[480];
  const HWND foreground = GetForegroundWindow();
  const bool ours = foreground == g_window;

  char who[128] = "";
  if (!ours) {
    // The class name only. Asking another process's window for its title is
    // a cross-process WM_GETTEXT that blocks until that process answers, and
    // an application that is busy or wedged never does. This runs on the
    // worker thread - the one carrying the watchdog and the hotkey that gets
    // the player out of trouble - so it is the last thread in the module that
    // may wait on somebody else's message loop.
    char cls[64] = "";
    GetClassNameA(foreground, cls, sizeof(cls));
    std::snprintf(who, sizeof(who), " foreground=%s", cls);
  }

  bool controls = false;
  const bool controls_known = game::ControlsDisabled(&controls);
  const bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
  const bool a_ = (GetAsyncKeyState('A') & 0x8000) != 0;
  const bool s_ = (GetAsyncKeyState('S') & 0x8000) != 0;
  const bool d = (GetAsyncKeyState('D') & 0x8000) != 0;

  // How far he actually travelled since this was last asked. With the keys
  // held beside it, this is the whole bug in one field: keys down and nothing
  // moving is the input being gone, recorded rather than reported.
  static bool  had_last = false;
  static float last_x = 0, last_y = 0, last_z = 0;
  static unsigned last_line_serial = 0;
  float px = 0, py = 0, pz = 0;
  char moved[32] = " moved=?";
  const unsigned line_serial = LastPositionSerial();
  if (line_serial == last_line_serial) {
    std::snprintf(moved, sizeof(moved), " moved=stale");
  } else if (LastLocalPosition(&px, &py, &pz)) {
    last_line_serial = line_serial;
    if (had_last) {
      const float dx = px - last_x, dy = py - last_y, dz = pz - last_z;
      std::snprintf(moved, sizeof(moved), " moved=%.2f",
                    std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    had_last = true;
    last_x = px;
    last_y = py;
    last_z = pz;
  }

  const std::string chain = ChainState();
  std::snprintf(text, sizeof(text),
                "focus=%s%s mode=%s cursor=%s controls=%s movement=%s "
                "keys=%s%s%s%s%s chain=%s wmkeys=%llu/%llu",
                ours ? "game" : "OTHER", who, ModeName(),
                CursorHook::freed() ? "ours" : "the game's",
                !controls_known ? "?" : controls ? "DISABLED" : "enabled",
                game::Enabled() ? "armed" : "off", w ? "W" : "",
                a_ ? "A" : "", s_ ? "S" : "", d ? "D" : "", moved,
                chain.c_str(),
                static_cast<unsigned long long>(g_keys_head.load()),
                static_cast<unsigned long long>(g_keys_witness.load()));
  // And every stage between the key and the character, read off the game,
  // and the mouse's own account.
  return std::string(text) +
         game::InputPipelineBrief(LastLocalPedPointer(), g_window) +
         game::MouseWatchLine() + samp::InputSwitchLine() + game::WatchpointLine() +
         game::ApiTraceLine() + game::col::Line();
}

void Overlay::WatchForLostInput() {
  // The layout, from here as well as from the panel: the panel only draws
  // when it is on screen, and a character who cannot press a letter is
  // stuck whether anybody is watching or not.
  if (CyrillicLayout()) AskForALatinLayout();

  // The moment the calls are armed, the whole picture as it was while
  // everything still worked - so the one taken when it stops has something
  // to be read against.
  static bool was_armed = false;
  const bool armed = game::Enabled();
  if (armed && !was_armed) {
    LOG_INFO("armed: baseline{}", game::InputPipelineFull(LastLocalPedPointer(),
                                                           g_window));
    // No code check and no thread snapshot here any more: both ran within
    // a quarter second of every lock that followed an arming, and a
    // protection that watches for scanners would see exactly those.
  }
  was_armed = armed;
  if (!armed) return;
  if (GetForegroundWindow() != g_window) return;
  const bool held = (GetAsyncKeyState('W') & 0x8000) ||
                    (GetAsyncKeyState('A') & 0x8000) ||
                    (GetAsyncKeyState('S') & 0x8000) ||
                    (GetAsyncKeyState('D') & 0x8000);
  // Not in a car: the keys drive there, the character does not walk, and
  // the watchdog would call a red light a lost keyboard.
  {
    const std::uintptr_t ped = LastLocalPedPointer();
    int state = 0;
    if (ped != 0 && mem::Read<int>(ped + 0x530, &state) && state == 50) return;
  }

  static float last_x = 0, last_y = 0, last_z = 0;
  static bool  had = false;
  static int   still = 0;
  static unsigned last_serial = 0;
  float x = 0, y = 0, z = 0;
  if (!LastLocalPosition(&x, &y, &z)) return;

  // A position nobody has refreshed says nothing about whether he moved. When
  // reading the player starts failing, the last one read stays exactly where
  // it was and is indistinguishable from a man standing still - which would
  // have this disarm the calls for a fault that is not there.
  const unsigned serial = LastPositionSerial();
  if (serial == last_serial) return;
  last_serial = serial;

  if (!had) {
    had = true;
    last_x = x; last_y = y; last_z = z;
    return;
  }
  const float dx = x - last_x, dy = y - last_y, dz = z - last_z;
  const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
  last_x = x; last_y = y; last_z = z;

  // A key down and nowhere gone. One sample is standing against a wall; six
  // in a row, a second and a half, is the input not arriving.
  still = (held && moved < 0.05f) ? still + 1 : 0;
  // Early, while it may still be happening: the picture at the third sample.
  if (still == 3)
    LOG_WARN("input suspect - keys held, not moving:{}",
             game::InputPipelineFull(LastLocalPedPointer(), g_window));
  if (still < 6) return;
  still = 0;

  // Said, not acted on. This was written when the module wrote into the pad
  // and the client answered by taking the input away: disarming was the only
  // way out. It presses the player's own keys now, and a character held up
  // against a wall, stepping round something or wedged in a corner looks
  // exactly like this - so standing the journey down for it threw away good
  // walks and left the journey waiting on a switch nobody had touched.
  // The walk has its own stuck handling; this only writes down what it saw.
  static int said = 0;
  if (said >= 3) return;
  ++said;
  bool controls = false;
  const bool known = game::ControlsDisabled(&controls);
  LOG_WARN("keys held and he has not moved for a second and a half. The game "
           "says its player controls are {}. Calls since arming: {} ground, "
           "{} line of sight, {} screen. Left alone - the walk decides what to "
           "do about being stuck",
           !known ? "unreadable" : controls ? "DISABLED" : "enabled",
           game::GroundCalls(), game::LineOfSightCalls(), game::ScreenCalls());
  LOG_WARN("not moving: picture{}",
           game::InputPipelineFull(LastLocalPedPointer(), g_window));
}

unsigned long long Overlay::KeyMessages() {
  return g_keys_head.load(std::memory_order_relaxed);
}

void Overlay::Disarm() {
  act::CancelTravel("stopped by hotkey");
  act::Stop("stopped by hotkey");
  game::SetEnabled(false);
  g_show_fan   = false;
  g_show_nodes = false;
  nav::ClearDebug();
  SetMode(Mode::kClosed);
  LOG_INFO("disarmed by hotkey: panel closed, cursor returned, movement off");
}

void Overlay::Shutdown() {
  Teardown();
  CursorHook::Uninstall();
}

void Overlay::DisableAfterFault() {
  CursorHook::SetFreed(false);
  // Deliberately does not tear ImGui down: whatever faulted may be mid-way
  // through its own state, and unwinding it now is another chance to crash.
  //
  // Nor is this permanent any more. A heavy diagnostic that faulted once cost
  // the panel for the rest of the session with no way back - but the fault was
  // in the work, not in the panel, and killing the interface over it was the
  // wrong trade. F11 brings it back.
  g_disabled = true;
  g_mode     = Mode::kClosed;
  LOG_ERROR("overlay faulted while drawing - hidden; press F11 to bring it back");
}

bool Overlay::disabled() { return g_disabled; }

bool Overlay::visible() { return g_hud || g_mode != Mode::kClosed; }

bool Overlay::MenuOpen() { return g_mode == Mode::kMenu; }

void Overlay::SetVisible(bool visible) {
  g_hud = visible;
  if (!visible && g_mode != Mode::kClosed) SetMode(Mode::kClosed);
}

}  // namespace gtabot::asi
