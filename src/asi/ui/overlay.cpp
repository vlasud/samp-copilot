#include "ui/overlay.hpp"

#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <spdlog/common.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "hooks/cursor.hpp"
#include "hooks/frame.hpp"
#include "log.hpp"
#include "mcp/rpc.hpp"
#include "samp/chat.hpp"
#include "samp/discovery.hpp"
#include "samp/version.hpp"
#include "samp/world.hpp"
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
constexpr int kLogLines  = 14;
constexpr int kChatLines = 6;
// Rebuilding the status json costs allocations; at 96 fps that is pure waste
// for numbers a human reads. Refresh it four times a second instead.
constexpr unsigned long long kRefreshMs = 250;

// Hidden -> passive -> interactive, cycled with one key so no second binding
// has to be found that GTA and SA-MP have both left alone.
enum class Mode { kHidden, kPassive, kInteractive };

Mode               g_mode        = Mode::kPassive;
bool               g_initialised = false;
bool               g_disabled    = false;
IDirect3DDevice9*  g_device      = nullptr;
HWND               g_window      = nullptr;
bool               g_toggle_down = false;
bool               g_resources_live = false;
WNDPROC            g_previous_wndproc = nullptr;
std::string        g_ini_path;

const ImVec4 kGreen{0.45f, 0.85f, 0.45f, 1.0f};
const ImVec4 kAmber{0.95f, 0.75f, 0.30f, 1.0f};
const ImVec4 kRed  {0.95f, 0.40f, 0.40f, 1.0f};
const ImVec4 kGrey {0.60f, 0.60f, 0.60f, 1.0f};

// Results of the posted diagnostics: written by a task on the game thread,
// read by the panel on a later frame.
std::mutex  g_summary_mutex;
std::string g_probe_summary;
std::string g_report_summary;

void SetProbeSummary(std::string text) {
  std::lock_guard<std::mutex> lock(g_summary_mutex);
  g_probe_summary = std::move(text);
}
std::string ProbeSummary() {
  std::lock_guard<std::mutex> lock(g_summary_mutex);
  return g_probe_summary;
}
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
  ImGui::SameLine(150.0f);
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

// Window messages only reach ImGui while the panel is interactive, so the game
// keeps its input in every other state.
LRESULT CALLBACK HookedWndProc(HWND window, UINT message, WPARAM wparam,
                               LPARAM lparam) {
  if (g_mode == Mode::kInteractive && ImGui::GetCurrentContext()) {
    ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse && IsMouseMessage(message)) return TRUE;
    if (io.WantCaptureKeyboard && IsKeyboardMessage(message)) return TRUE;
  }
  return CallWindowProcW(g_previous_wndproc, window, message, wparam, lparam);
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

  // The built-in font has ASCII glyphs and nothing else, so every Cyrillic
  // letter in the chat would draw as a box - which makes the panel useless
  // for checking a read against what is on screen, on the one server we can
  // actually check against. Tahoma ships with Windows and covers Cyrillic.
  char fonts[MAX_PATH] = {};
  if (GetWindowsDirectoryA(fonts, MAX_PATH)) {
    const std::string path = std::string(fonts) + "\\Fonts\\tahoma.ttf";
    if (io.Fonts->AddFontFromFileTTF(path.c_str(), 15.0f, nullptr,
                                     io.Fonts->GetGlyphRangesCyrillic()) ==
        nullptr)
      LOG_WARN("no {} - the panel cannot draw Cyrillic", path);
  }

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
  const bool down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
  if (down && !g_toggle_down) {
    switch (g_mode) {
      case Mode::kPassive:     g_mode = Mode::kInteractive; break;
      case Mode::kInteractive: g_mode = Mode::kHidden;      break;
      case Mode::kHidden:      g_mode = Mode::kPassive;     break;
    }
    // ImGui draws the cursor itself: the game hides the system one, so there
    // would otherwise be nothing to aim with.
    ImGui::GetIO().MouseDrawCursor = g_mode == Mode::kInteractive;
    // And the pointer changes hands here, in the one place the mode changes.
    CursorHook::SetFreed(g_mode == Mode::kInteractive);
  }
  g_toggle_down = down;
}

void DrawPanel() {
  static json               cached;
  static json               cached_chat;
  static unsigned long long cached_at = 0;
  const unsigned long long now = GetTickCount64();
  if (cached.is_null() || now - cached_at >= kRefreshMs) {
    cached    = BuildStatusSnapshot();
    // Cached alongside the status for the same reason: reading the ring means
    // validating an address per entry, and doing that ninety times a second
    // for six lines a person is reading is pure waste.
    cached_chat = samp::CachedChat().valid ? samp::ReadChat(kChatLines)
                                           : json::object();
    cached_at = now;
  }

  const json& status = cached;
  const json frame  = status.value("frame", json::object());
  const json hook   = status.value("hook", json::object());
  const std::string verdict = status.value("verdict", std::string{"?"});

  // FirstUseEver, not Always: past the first run the position comes from
  // bot.imgui.ini, which is the point of being able to drag it.
  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(520, 430), ImGuiCond_FirstUseEver);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
  if (g_mode != Mode::kInteractive)
    flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoInputs;
  ImGui::Begin("gtabot", nullptr, flags);

  if (g_mode == Mode::kInteractive) {
    ImGui::TextColored(kAmber, "interactive - drag to move, F11 to hide");
    // Says plainly whether the pointer was actually taken. A counter that
    // stays at zero means something other than SetCursorPos is holding it,
    // and the log names what.
    if (!CursorHook::installed())
      ImGui::TextColored(kRed, "the mouse could not be taken from the game");
    else
      ImGui::TextColored(kGrey, "mouse held from the game (%llu recentres "
                                "answered)",
                         static_cast<unsigned long long>(
                             CursorHook::suppressed()));
  } else {
    ImGui::TextColored(kGrey, "F11 to grab the mouse");
  }

  const bool healthy = verdict == "ok";
  Label("verdict", verdict, healthy ? kGreen : kAmber);

  ImGui::Separator();
  Label("frames", std::to_string(frame.value("frames", 0ull)));
  Label("fps", std::to_string(static_cast<int>(frame.value("fps", 0.0))));
  Label("idle", std::to_string(frame.value("idle_ms", 0ull)) + " ms");
  Label("driver", frame.value("driver", std::string{"?"}));
  const bool intact = hook.value("present_intact", false) ||
                      hook.value("endscene_intact", false);
  Label("patch bytes", intact ? "intact" : "REWRITTEN",
        intact ? kGreen : kRed);

  ImGui::Separator();
  const samp::Client client = samp::Detect();
  Label("samp.dll", client.base ? samp::ToString(client.version) : "not loaded",
        client.base ? kGreen : kGrey);
  if (client.base) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%08X",
                  static_cast<unsigned int>(client.base));
    Label("base", buffer);
  }

  ImGui::Separator();
  const samp::Layout& layout = samp::ResolveLayout();
  if (layout.valid) {
    char pool_text[32];
    std::snprintf(pool_text, sizeof(pool_text), "0x%08X",
                  static_cast<unsigned>(layout.player_pool));
    Label("pool", pool_text, kGreen);
    Label("host", layout.host, kGreen);
  } else {
    Label("pool", layout.note.empty() ? "not resolved" : layout.note, kAmber);
  }

  ImGui::Separator();
  // The chat, drawn next to the real one on purpose: the only way to know a
  // column was labelled right is to read it against what is on screen.
  const samp::ChatLayout& chat = samp::CachedChat();
  if (chat.valid) {
    char shape[96];
    std::snprintf(shape, sizeof(shape), "%d lines, stride %u, %s, %s",
                  chat.populated, chat.stride,
                  chat.anchored ? "anchored" : "shape only",
                  chat.order_known
                      ? (chat.newest_first ? "newest first" : "oldest first")
                      : "order unknown");
    Label("chat", shape, kGreen);
    for (const json& line : cached_chat.value("lines", json::array())) {
      const std::string from = line.value("from", std::string{});
      ImGui::TextWrapped("  %s%s", from.empty() ? "" : (from + "  ").c_str(),
                         line.value("text", std::string{}).c_str());
    }
  } else {
    Label("chat", chat.note.empty() ? "not resolved" : chat.note, kAmber);
  }
  if (g_mode == Mode::kInteractive && ImGui::Button("Dump chat")) {
    Bridge::PostToGameThread([]() { samp::DumpChat(); });
  }

  ImGui::Separator();
  const StatusSource::Mcp mcp = StatusSource::mcp();
  Label("mcp", mcp.listening ? mcp.endpoint : "not listening",
        mcp.listening ? kGreen : kRed);
  Label("requests", std::to_string(mcp.requests));
  Label("in flight", std::to_string(mcp::Rpc::in_flight()) + "  timed out " +
                         std::to_string(mcp::Rpc::timed_out()));

  const std::int64_t world_age_ms = Bridge::world_age_ms();
  Label("world age", world_age_ms < 0 ? "never built"
                                      : std::to_string(world_age_ms) + " ms");
  Label("task queue", std::to_string(Bridge::pending_tasks()) + " pending, " +
                          std::to_string(Bridge::dropped_tasks()) + " dropped");

  ImGui::Separator();
  // The panel draws on the game thread, so a probe can simply be run here -
  // no round trip, and no need to be alt-tabbed away to ask for one.
  static std::string probe_summary;
  if (g_mode == Mode::kInteractive) {
    // Posted rather than run here. A scan of the whole process inside the
    // draw call charges any fault in it to the panel, and the panel is what
    // gets switched off for it.
    if (ImGui::Button("Run memory probe")) {
      SetProbeSummary("running...");
      Bridge::PostToGameThread([]() {
        try {
          const json result = ProbeMemory(json::object());
          LogProbeSummary(result);
          const json search = result.value("search", json::object());
          SetProbeSummary(search.value("needle", std::string{"<none>"}) +
                          " found " +
                          std::to_string(search.value("found", std::size_t{0})) +
                          " time(s)");
        } catch (const std::exception& e) {
          SetProbeSummary(std::string("failed: ") + e.what());
        }
      });
    }
    probe_summary = ProbeSummary();
    if (!probe_summary.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(kGrey, "%s", probe_summary.c_str());
    }

    // The launcher nickname is useless as an anchor on a roleplay server,
    // where the name above the character is a different string entirely. It
    // has to be typed, and typing it here beats alt-tabbing to do it.
    static char        needle[64] = "";
    static std::string report_summary;
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##needle", "name shown above your character",
                             needle, sizeof(needle));
    ImGui::SameLine();
    if (ImGui::Button("Dump SA-MP structures")) {
      SetReportSummary("running...");
      const std::string wanted = needle;
      Bridge::PostToGameThread([wanted]() {
        samp::DumpPlayerRecords();
        const samp::ReportOutcome outcome = samp::WriteStructureReport(wanted);
        SetReportSummary(outcome.written
                             ? "wrote " + outcome.path + " - " +
                                   std::to_string(outcome.structure_hits) +
                                   " worth looking at, " +
                                   std::to_string(outcome.command_line_hits) +
                                   " command-line copies skipped"
                             : outcome.error);
      });
    }
    report_summary = ReportSummary();
    if (!report_summary.empty())
      ImGui::TextWrapped("%s", report_summary.c_str());
  }

  ImGui::Separator();
  ImGui::TextColored(kGrey, "log");
  ImGui::BeginChild("log", ImVec2(0, kLogLines * ImGui::GetTextLineHeight()),
                    false, ImGuiWindowFlags_NoInputs);
  for (const LogLine& line : RecentLogLines(kLogLines)) {
    ImGui::TextColored(LevelColour(line.level), "%s", line.text.c_str());
  }
  ImGui::EndChild();

  ImGui::End();
}

}  // namespace

void Overlay::Render(IDirect3DDevice9* device) {
  if (!device) return;
  if (g_disabled) {
    // The key is still polled, so a way back exists.
    const bool down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
    if (down && !g_toggle_down) {
      g_disabled = false;
      g_mode     = Mode::kPassive;
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

  PollToggle();
  if (g_mode == Mode::kHidden) return;

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
  if (g_mode == Mode::kInteractive && CursorHook::freed()) {
    POINT pointer{};
    if (CursorHook::RealCursorPos(&pointer) &&
        ScreenToClient(g_window, &pointer))
      ImGui::GetIO().AddMousePosEvent(static_cast<float>(pointer.x),
                                      static_cast<float>(pointer.y));
  }
  ImGui::NewFrame();
  DrawPanel();
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
  g_mode     = Mode::kHidden;
  LOG_ERROR("overlay faulted while drawing - hidden; press F11 to bring it back");
}

bool Overlay::disabled() { return g_disabled; }

bool Overlay::visible() { return g_mode != Mode::kHidden; }

void Overlay::SetVisible(bool visible) {
  g_mode = visible ? Mode::kPassive : Mode::kHidden;
  if (ImGui::GetCurrentContext()) ImGui::GetIO().MouseDrawCursor = false;
}

}  // namespace gtabot::asi
