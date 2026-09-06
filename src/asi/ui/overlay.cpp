#include "ui/overlay.hpp"

#include <windows.h>
#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <spdlog/common.h>

#include <string>
#include <vector>

#include "bridge.hpp"
#include "hooks/frame.hpp"
#include "log.hpp"
#include "mcp/rpc.hpp"
#include "samp/version.hpp"
#include "state/probe.hpp"
#include "ui/status_source.hpp"

namespace gtabot::asi {
namespace {

// F9: F8 is GTA's own screenshot key.
constexpr int kToggleKey = VK_F9;
constexpr int kLogLines  = 14;
// Rebuilding the status json costs allocations; at 96 fps that is pure waste
// for numbers a human reads. Refresh it four times a second instead.
constexpr unsigned long long kRefreshMs = 250;

bool               g_initialised = false;
bool               g_visible     = true;
bool               g_disabled    = false;
IDirect3DDevice9*  g_device      = nullptr;
HWND               g_window      = nullptr;
bool               g_toggle_down = false;

const ImVec4 kGreen{0.45f, 0.85f, 0.45f, 1.0f};
const ImVec4 kAmber{0.95f, 0.75f, 0.30f, 1.0f};
const ImVec4 kRed  {0.95f, 0.40f, 0.40f, 1.0f};
const ImVec4 kGrey {0.60f, 0.60f, 0.60f, 1.0f};

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
  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_initialised = false;
  g_device      = nullptr;
  g_window      = nullptr;
}

bool Initialise(IDirect3DDevice9* device) {
  D3DDEVICE_CREATION_PARAMETERS params{};
  if (FAILED(device->GetCreationParameters(&params))) return false;
  g_window = params.hFocusWindow;
  if (!g_window) return false;

  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename  = nullptr;  // no imgui.ini dropped into the game folder
  io.LogFilename  = nullptr;
  // The panel is read-only, so ImGui must never believe it owns the cursor.
  io.MouseDrawCursor = false;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().WindowRounding = 4.0f;
  ImGui::GetStyle().Alpha          = 0.92f;

  if (!ImGui_ImplWin32_Init(g_window)) return false;
  if (!ImGui_ImplDX9_Init(device)) {
    ImGui_ImplWin32_Shutdown();
    return false;
  }

  g_device      = device;
  g_initialised = true;
  LOG_INFO("overlay initialised on hwnd 0x{:08X} (F9 toggles it)",
           reinterpret_cast<std::uintptr_t>(g_window));
  return true;
}

void PollToggle() {
  const bool down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
  if (down && !g_toggle_down) g_visible = !g_visible;
  g_toggle_down = down;
}

void DrawPanel() {
  static json               cached;
  static unsigned long long cached_at = 0;
  const unsigned long long now = GetTickCount64();
  if (cached.is_null() || now - cached_at >= kRefreshMs) {
    cached    = BuildStatusSnapshot();
    cached_at = now;
  }

  const json& status = cached;
  const json frame  = status.value("frame", json::object());
  const json hook   = status.value("hook", json::object());
  const std::string verdict = status.value("verdict", std::string{"?"});

  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Always);
  ImGui::Begin("gtabot", nullptr,
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_AlwaysAutoResize);

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
  if (!device || g_disabled) return;

  // The game can recreate its device outright rather than resetting it, which
  // leaves the backend pointing at a dead object.
  if (g_initialised && device != g_device) {
    LOG_WARN("d3d9 device changed, rebuilding the overlay");
    Teardown();
  }
  if (!g_initialised && !Initialise(device)) return;

  PollToggle();
  if (!g_visible) return;

  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  DrawPanel();
  ImGui::EndFrame();
  ImGui::Render();
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void Overlay::OnLostDevice() {
  if (g_initialised) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void Overlay::OnResetDevice() {
  if (g_initialised) ImGui_ImplDX9_CreateDeviceObjects();
}

void Overlay::Shutdown() { Teardown(); }

void Overlay::DisableAfterFault() {
  // Deliberately does not tear ImGui down: whatever faulted may be mid-way
  // through its own state, and unwinding it now is another chance to crash.
  g_disabled = true;
  g_visible  = false;
  LOG_ERROR("overlay faulted while drawing - switched off for this session, "
            "the rest of the module keeps running");
}

bool Overlay::disabled() { return g_disabled; }

bool Overlay::visible() { return g_visible; }

void Overlay::SetVisible(bool visible) { g_visible = visible; }

}  // namespace gtabot::asi
