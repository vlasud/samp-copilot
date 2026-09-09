#include "samp/keys.hpp"

#include "game/mouse_watch.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace gtabot::samp {
namespace {

// A character arrives as a message and is read whenever the game gets to
// it. A key is different: the game samples the keyboard once a frame and
// turns it into the pad, so one pressed and let go inside a single frame is
// a key that was never down. They are held for a few frames instead.
enum class Kind { kCharacter, kKeyDown, kKeyUp, kWait };
constexpr int kHoldFrames = 3;

struct Step {
  Kind    kind = Kind::kWait;
  wchar_t ch = 0;
  int     vk = 0;
};

std::mutex g_mutex;
std::vector<Step> g_script;
std::size_t g_at = 0;
std::vector<int> g_held;
std::atomic<unsigned long long> g_events{0};
std::atomic<unsigned long long> g_last_event_ms{0};
// What this module currently has down, without a lock. The panic release
// runs on a thread that must never wait for the game thread - the whole
// reason it runs is that the game thread has stopped - so it cannot ask the
// script what it was doing.
std::atomic<bool> g_down[256] = {};

void Wipe() {
  if (!g_script.empty())
    SecureZeroMemory(g_script.data(), g_script.size() * sizeof(Step));
  g_script.clear();
  g_at = 0;
}

// A character, put into the window's own message queue.
//
// `KEYEVENTF_UNICODE` looks like it should do this - the scan field carries
// the code point and Windows delivers it as typed - but the game's window is
// an ANSI one, so what arrives is not the code point: Windows first folds it
// down to a single byte using the code page of whatever keyboard layout the
// window's thread currently has. This module keeps that layout English so
// that W and S reach the game at all, and English is code page 1252, which
// has no Russian in it. Every Cyrillic letter therefore arrived as a question
// mark - the character said "добрый день господа" in chat and the server
// showed "?????? ???? ???????".
//
// So the fold is done here instead, to CP1251, which is what a Russian server
// speaks and what the client stores, and the byte is posted straight to the
// window. Nothing is left for a layout to reinterpret.
void SendCharacter(wchar_t ch) {
  const HWND window = game::GameWindow();
  g_last_event_ms.store(GetTickCount64());
  if (window == nullptr) {
    // No window to post to: the old way, which at least still types Latin.
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wScan = static_cast<WORD>(ch);
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1] = in[0];
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    return;
  }
  char byte = '?';
  const int made = WideCharToMultiByte(1251, 0, &ch, 1, &byte, 1, "?", nullptr);
  if (made != 1) return;
  PostMessageA(window, WM_CHAR, static_cast<WPARAM>(
                   static_cast<unsigned char>(byte)), 1);
}

void SendKey(int vk, bool down) {
  if (vk >= 0 && vk < 256) g_down[vk].store(down);
  g_last_event_ms.store(GetTickCount64());

  // With the window behind another, a synthesised keystroke goes wherever
  // the focus is - somebody's browser, their editor. That is useless to the
  // character and rude to them. The game reads its keyboard from its own
  // window messages, which is the same path the typed characters take, so
  // the key is posted to the window and reaches nothing else.
  const HWND window = game::GameWindow();
  if (window != nullptr && GetForegroundWindow() != window) {
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LPARAM info = static_cast<LPARAM>(1) | (static_cast<LPARAM>(scan) << 16);
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
        vk == VK_RMENU || vk == VK_RCONTROL || vk == VK_INSERT ||
        vk == VK_DELETE || vk == VK_HOME || vk == VK_END)
      info |= 0x01000000;                     // an extended key
    if (!down) info |= 0xC0000000;            // it was down, and is going up
    // Alt is a system key and arrives as one, or the game does not see it.
    const bool alt = vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
    UINT what = down ? WM_KEYDOWN : WM_KEYUP;
    if (alt) what = down ? WM_SYSKEYDOWN : WM_SYSKEYUP;
    PostMessageA(window, what, static_cast<WPARAM>(vk), info);
    return;
  }

  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = static_cast<WORD>(vk);
  in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(vk),
                                                 MAPVK_VK_TO_VSC));
  if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)
    in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
  SendInput(1, &in, sizeof(in));
}

}  // namespace

void KeysTypeWide(const std::wstring& text) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (wchar_t ch : text) g_script.push_back(Step{Kind::kCharacter, ch, 0});
}

void KeysType(const std::string& utf8) {
  if (utf8.empty()) return;
  const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                       static_cast<int>(utf8.size()), nullptr, 0);
  if (need <= 0) return;
  std::wstring wide(static_cast<std::size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), need);
  KeysTypeWide(wide);
  SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
}

void KeysPress(int virtual_key, int times) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (int i = 0; i < times; ++i) {
    g_script.push_back(Step{Kind::kKeyDown, 0, virtual_key});
    for (int f = 0; f < kHoldFrames; ++f)
      g_script.push_back(Step{Kind::kWait, 0, 0});
    g_script.push_back(Step{Kind::kKeyUp, 0, virtual_key});
    g_script.push_back(Step{Kind::kWait, 0, 0});
  }
}

void KeysHold(const std::vector<int>& keys) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (int held : g_held)
    if (std::find(keys.begin(), keys.end(), held) == keys.end()) {
      SendKey(held, false);
      g_events.fetch_add(1, std::memory_order_relaxed);
    }
  for (int want : keys)
    if (std::find(g_held.begin(), g_held.end(), want) == g_held.end()) {
      SendKey(want, true);
      g_events.fetch_add(1, std::memory_order_relaxed);
    }
  g_held = keys;
}

void KeysReleaseAll() { KeysHold({}); }

void KeysPanicRelease() {
  for (int vk = 0; vk < 256; ++vk) {
    if (!g_down[vk].load()) continue;
    SendKey(vk, false);
  }
}

unsigned long long KeysLastEventMs() {
  return g_last_event_ms.load();
}

unsigned long long KeysEventsSent() {
  return g_events.load(std::memory_order_relaxed);
}

void KeysPressFor(int virtual_key, int frames) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_script.push_back(Step{Kind::kKeyDown, 0, virtual_key});
  for (int f = 0; f < frames; ++f)
    g_script.push_back(Step{Kind::kWait, 0, 0});
  g_script.push_back(Step{Kind::kKeyUp, 0, virtual_key});
}

void KeysTick() {
  Step step;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_at >= g_script.size()) {
      if (!g_script.empty()) Wipe();
      return;
    }
    step = g_script[g_at++];
  }
  switch (step.kind) {
    case Kind::kCharacter: SendCharacter(step.ch); break;
    case Kind::kKeyDown:   SendKey(step.vk, true); break;
    case Kind::kKeyUp:     SendKey(step.vk, false); break;
    case Kind::kWait:      break;
  }
  if (step.kind != Kind::kWait) g_events.fetch_add(1, std::memory_order_relaxed);
}

bool KeysBusy() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_at < g_script.size();
}

void KeysClear() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Wipe();
}

}  // namespace gtabot::samp
