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

std::once_flag  g_once;
std::atomic<bool> g_enabled{true};
int g_width  = 1280;
int g_height = 720;
// The window is styled once. Restyling every reset fights the game's own
// SetWindowPos and makes the window flicker and jump.
std::atomic<bool> g_styled{false};

void ReadConfig() {
  const std::string path = ModuleDirectory() + "bot.cfg";
  std::ifstream file(path);
  if (!file) {
    LOG_INFO("no bot.cfg - windowed mode on at {}x{} (default); write "
             "'window=off' there to disable", g_width, g_height);
    return;
  }
  std::string line;
  while (std::getline(file, line)) {
    // Trim and split on '='.
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string s) {
      const std::size_t a = s.find_first_not_of(" \t\r\n");
      const std::size_t b = s.find_last_not_of(" \t\r\n");
      return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
    };
    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));
    if (key != "window") continue;

    if (value == "off" || value == "0" || value == "false") {
      g_enabled.store(false);
      LOG_INFO("bot.cfg: windowed mode off - the game stays fullscreen");
      return;
    }
    int w = 0, h = 0;
    if (std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 &&
        h >= 240 && w <= 7680 && h <= 4320) {
      g_width  = w;
      g_height = h;
      LOG_INFO("bot.cfg: windowed mode at {}x{}", g_width, g_height);
    } else {
      LOG_WARN("bot.cfg: 'window={}' is not off or WIDTHxHEIGHT - using {}x{}",
               value, g_width, g_height);
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

void WindowMode::ForceWindowed(D3DPRESENT_PARAMETERS* params) {
  if (!Enabled() || params == nullptr) return;
  params->Windowed = TRUE;
  // A refresh rate is only legal for a fullscreen device; leaving the game's
  // fullscreen value here makes the windowed CreateDevice fail outright.
  params->FullScreen_RefreshRateInHz = 0;
  params->BackBufferWidth  = static_cast<UINT>(g_width);
  params->BackBufferHeight = static_cast<UINT>(g_height);
}

void WindowMode::ApplyWindowStyle(HWND window) {
  if (!Enabled() || window == nullptr) return;
  if (g_styled.exchange(true)) return;

  const LONG style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
  SetWindowLongPtrW(window, GWL_STYLE, style);
  // Also drop any extended top-most/fullscreen style the game may have set.
  LONG ex = GetWindowLongW(window, GWL_EXSTYLE);
  ex &= ~(WS_EX_TOPMOST);
  SetWindowLongPtrW(window, GWL_EXSTYLE, ex);

  RECT rect{0, 0, g_width, g_height};
  AdjustWindowRect(&rect, style, FALSE);
  const int ww = rect.right - rect.left;
  const int wh = rect.bottom - rect.top;
  const int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
  const int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
  SetWindowPos(window, HWND_NOTOPMOST, sx < 0 ? 0 : sx, sy < 0 ? 0 : sy, ww, wh,
               SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  LOG_INFO("window styled to {}x{} client, centred", g_width, g_height);
}

}  // namespace gtabot::asi
