#include "ui/overlay.hpp"

#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <spdlog/common.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "game/exe.hpp"
#include "game/paths.hpp"
#include "game/world_query.hpp"
#include "hooks/cursor.hpp"
#include "hooks/frame.hpp"
#include "log.hpp"
#include "mcp/rpc.hpp"
#include "nav/planner.hpp"
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
// The log column is as tall as the window, so it holds a lot more than the
// fourteen lines it used to get at the bottom of one.
constexpr int   kLogLines   = 60;
constexpr int   kChatLines  = 8;
constexpr float kLeftColumn = 400.0f;
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

// ---- movement debugging ----------------------------------------------------

constexpr int   kFanSpokes   = 16;
constexpr float kFanMetres   = 8.0f;
constexpr float kNodeRadius  = 80.0f;
constexpr unsigned long long kFanRefreshMs  = 1000;
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
                                          kNodeRadius, 300));
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
void DrawWorld() {
  if (!game::CallsTrusted()) return;
  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) return;

  ImDrawList* draw = ImGui::GetBackgroundDrawList();
  const nav::DebugState debug = nav::GetDebug();

  float px = 0, py = 0;
  const bool self_on_screen =
      game::ToScreen(game::Vec3{self.x, self.y, self.z - 0.9f}, &px, &py);

  if (g_show_nodes) {
    for (const game::PathNode& node : debug.nodes) {
      float sx = 0, sy = 0;
      if (!game::ToScreen(node.pos, &sx, &sy)) continue;
      draw->AddCircleFilled(ImVec2(sx, sy), 3.0f, kDotNode);
    }
  }

  if (g_show_fan && self_on_screen) {
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
      if (!leg.ok && !leg.why.empty())
        Shadowed(draw, ImVec2(bx + 8, by - 8), kLineBlocked, leg.why.c_str());
    }
    float tx = 0, ty = 0;
    const game::Vec3 target{debug.target.x, debug.target.y, debug.target.z - 0.9f};
    if (game::ToScreen(target, &tx, &ty)) {
      draw->AddCircle(ImVec2(tx, ty), 9.0f, kDotTarget, 16, 2.0f);
      char label[96];
      const float dx = debug.target.x - self.x;
      const float dy = debug.target.y - self.y;
      std::snprintf(label, sizeof(label), "%.1f m  %s",
                    std::sqrt(dx * dx + dy * dy),
                    plan.ok ? "ok" : plan.note.c_str());
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
  if (g_mode != Mode::kInteractive)
    flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoInputs;
  ImGui::Begin("gtabot", nullptr, flags);

  // Left: what the bot can see. Right: what it has been saying. Everything
  // that was here to prove the plumbing works - frame counters, patch bytes,
  // queue depths, pool addresses - has gone: it is answered by the verdict
  // when it matters and is noise when it does not.
  ImGui::BeginChild("left", ImVec2(kLeftColumn, 0), false);

  if (g_mode == Mode::kInteractive) {
    if (CursorHook::installed())
      ImGui::TextColored(kAmber, "interactive - the mouse is ours, F11 to "
                                 "give it back");
    else
      ImGui::TextColored(kRed, "interactive, but the mouse could not be taken "
                               "from the game");
  } else {
    ImGui::TextColored(kGrey, "F11 to grab the mouse");
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
    } else if (!game::CallsTrusted()) {
      Label("game", "calls not verified yet (on foot, on the ground?)", kAmber);
    } else {
      Label("game", "calls verified", kGreen);
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
      std::snprintf(text, sizeof(text), "%s - %s, %.1f m, %d calls",
                    debug.plan.ok ? "ok" : "NO", debug.plan.note.c_str(),
                    debug.plan.length_m, debug.plan.game_calls);
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
                    g_mode == Mode::kInteractive ? "interactive" : "passive",
                    g_mode == Mode::kInteractive ? "ours" : "the game's",
                    (w || a || s_ || d) ? "  held:" : "", w ? " W" : "",
                    a ? " A" : "", s_ ? " S" : "", d ? " D" : "");
      Label("input", text, g_mode == Mode::kInteractive ? kAmber : kGrey);

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
      if (g_show_fan) {
        std::snprintf(text, sizeof(text), "%d calls, %llu ms per refresh",
                      g_fan_calls.load(),
                      static_cast<unsigned long long>(g_fan_ms.load()));
        Label("fan", text, g_fan_ms.load() > 30 ? kAmber : kGrey);
      }
    }

    if (g_mode == Mode::kInteractive) {
      if (ImGui::Button("Plan 15 m ahead")) PlanAhead(15.0f);
      ImGui::SameLine();
      if (ImGui::Button("Plan 60 m ahead")) PlanAhead(60.0f);
      ImGui::SameLine();
      if (ImGui::Button("Clear")) nav::ClearDebug();
      ImGui::Checkbox("fan", &g_show_fan);
      ImGui::SameLine();
      ImGui::Checkbox("nodes", &g_show_nodes);
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

  if (g_mode == Mode::kInteractive) {
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
                        (g_mode == Mode::kInteractive
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
  DrawWorld();
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
