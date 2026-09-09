#include "hooks/windowmode.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include "log.hpp"
#include "types.hpp"

namespace gtabot::asi {
namespace {

std::once_flag    g_once;
std::atomic<bool> g_enabled{true};
// Zero means "whatever resolution the game chose". Forcing a size here is what
// made the game start and immediately quit: RenderWare builds its camera and
// rasters from the video mode it selected, and a device whose back buffer is a
// different size fails that setup, so the game shuts itself down cleanly.
// The size is therefore opt-in and warned about; the supported way to get a
// small window is to pick a small resolution in the game's own display
// settings, which resets the device and comes back windowed.
int g_width  = 0;
int g_height = 0;

// The window is styled once. Restyling on every reset fights the game's own
// SetWindowPos and makes the window flicker and jump.
std::atomic<bool> g_styled{false};
std::atomic<bool> g_walker_allowed{true};
std::atomic<bool> g_diagnostics_allowed{false};
std::atomic<bool> g_gamepad_allowed{false};

std::string Trim(const std::string& in) {
  const std::size_t a = in.find_first_not_of(" \t\r\n");
  const std::size_t b = in.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string{} : in.substr(a, b - a + 1);
}

std::atomic<bool> g_background{true};

void ReadConfig() {
  const std::string path = ModuleDirectory() + "bot.cfg";
  std::ifstream file(path);
  if (!file) {
    LOG_INFO("no bot.cfg - windowed mode on at the game's own resolution; "
             "write 'window=off' there to disable");
    return;
  }
  std::string line;
  while (std::getline(file, line)) {
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key   = Trim(line.substr(0, eq));
    const std::string value = Trim(line.substr(eq + 1));
    if (key == "gamepad") {
      const bool on = value == "on" || value == "1" || value == "true";
      g_gamepad_allowed.store(on);
      LOG_INFO("bot.cfg: the gamepad is {}", on ? "allowed" : "ignored");
      continue;
    }
    if (key == "background") {
      const bool off = value == "off" || value == "0" || value == "false";
      g_background.store(!off);
      LOG_INFO("bot.cfg: the game {} while the window is behind another",
               off ? "stops" : "carries on");
      continue;
    }
    if (key == "diagnostics") {
      const bool on = value == "on" || value == "1" || value == "true";
      g_diagnostics_allowed.store(on);
      LOG_INFO("bot.cfg: diagnostics {}", on ? "on" : "off");
      continue;
    }
    if (key == "walker") {
      const bool off = value == "off" || value == "0" || value == "false";
      g_walker_allowed.store(!off);
      LOG_INFO("bot.cfg: the walker may {} patch the game's code",
               off ? "NOT" : "");
      continue;
    }
    if (key != "window") continue;

    if (value == "off" || value == "0" || value == "false") {
      g_enabled.store(false);
      LOG_INFO("bot.cfg: windowed mode off - the game stays fullscreen");
      return;
    }
    if (value == "on" || value == "1" || value == "true" || value.empty()) {
      LOG_INFO("bot.cfg: windowed mode on at the game's own resolution");
      return;
    }
    int w = 0, h = 0;
    if (std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 &&
        h >= 240 && w <= 7680 && h <= 4320) {
      g_width  = w;
      g_height = h;
      LOG_WARN("bot.cfg: window={}x{} overrides the game's own back buffer. "
               "The game quits at startup when that size disagrees with the "
               "video mode it picked - use 'window=on' and set the resolution "
               "in the game's display settings if it does", w, h);
    } else {
      LOG_WARN("bot.cfg: 'window={}' is not off, on, or WIDTHxHEIGHT - "
               "using the game's own resolution", value);
    }
    return;
  }
}

}  // namespace

void WindowMode::EnsureConfigured() { std::call_once(g_once, ReadConfig); }

bool WindowMode::Enabled() {
  EnsureConfigured();
  return g_enabled.load(std::memory_order_acquire);
}

bool WindowMode::DiagnosticsAllowed() {
  EnsureConfigured();
  return g_diagnostics_allowed.load();
}

bool WindowMode::RunsInBackground() {
  EnsureConfigured();
  return g_background.load();
}

bool WindowMode::WalkerAllowed() {
  EnsureConfigured();
  return g_walker_allowed.load(std::memory_order_acquire);
}

bool WindowMode::GamepadAllowed() {
  EnsureConfigured();
  return g_gamepad_allowed.load(std::memory_order_acquire);
}

void WindowMode::ForceWindowed(D3DPRESENT_PARAMETERS* params) {
  if (!Enabled() || params == nullptr) return;
  params->Windowed = TRUE;
  // A refresh rate is only legal for a fullscreen device; leaving the game's
  // fullscreen value here makes the windowed CreateDevice fail outright.
  params->FullScreen_RefreshRateInHz = 0;
  // The size is left alone unless it was asked for explicitly. See above.
  if (g_width > 0 && g_height > 0) {
    params->BackBufferWidth  = static_cast<UINT>(g_width);
    params->BackBufferHeight = static_cast<UINT>(g_height);
  }
}

void WindowMode::ApplyWindowStyle(HWND window, int width, int height) {
  if (!Enabled() || window == nullptr) return;
  if (g_styled.exchange(true)) return;

  const LONG style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
  SetWindowLongPtrW(window, GWL_STYLE, style);
  LONG ex = GetWindowLongW(window, GWL_EXSTYLE);
  ex &= ~WS_EX_TOPMOST;
  SetWindowLongPtrW(window, GWL_EXSTYLE, ex);

  if (width <= 0 || height <= 0) {
    // The device chose its own size from the window; leave the geometry and
    // just give it a frame.
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    LOG_INFO("window given a frame at its own size");
    return;
  }

  RECT rect{0, 0, width, height};
  AdjustWindowRect(&rect, style, FALSE);
  const int ww = rect.right - rect.left;
  const int wh = rect.bottom - rect.top;
  int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
  int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
  if (sx < 0) sx = 0;
  if (sy < 0) sy = 0;
  SetWindowPos(window, HWND_NOTOPMOST, sx, sy, ww, wh,
               SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  LOG_INFO("window styled to {}x{} client, centred", width, height);
}

}  // namespace gtabot::asi
