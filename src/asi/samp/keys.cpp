#include "samp/keys.hpp"

#include "game/mouse_watch.hpp"
#include "hooks/windowmode.hpp"

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
// How fast a person types, near enough, and when the last letter went.
// Twelve letters a second: fast for a hand, but a hand that types all day.
// A quarter of a second was right about hands and unusable - forty
// characters took ten seconds to say, and by then the brain had decided to
// say it again.
constexpr unsigned long long kBetweenLettersMs = 83;
unsigned long long g_last_letter_ms = 0;
// And how each one went down: as a message to the window, or through the
// system. A key must be let go of the same way it was pressed. Choosing the
// route afresh for the release is what left keys stuck: pressed through the
// system with the game in front, then released as a message once somebody
// alt-tabbed away - the game let go, and Windows went on believing the key
// was held, in every other program on the desktop.
std::atomic<bool> g_by_message[256] = {};

// Whether a key pressed now would go as a message rather than through the
// system: only when the game is not the window in front, and only when it
// is meant to carry on back there.
bool WouldPost() {
  const HWND window = game::GameWindow();
  if (window == nullptr) return false;
  if (GetForegroundWindow() == window) return false;
  return asi::WindowMode::RunsInBackground();
}

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
  // Down: pick the route and remember it. Up: whatever the route was.
  bool post = false;
  if (down) {
    post = WouldPost();
    if (vk >= 0 && vk < 256) g_by_message[vk].store(post);
  } else {
    post = vk >= 0 && vk < 256 && g_by_message[vk].load();
  }
  if (post && window != nullptr) {
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LPARAM info = static_cast<LPARAM>(1) | (static_cast<LPARAM>(scan) << 16);
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
        vk == VK_RMENU || vk == VK_RCONTROL || vk == VK_INSERT ||
        vk == VK_DELETE || vk == VK_HOME || vk == VK_END)
      info |= 0x01000000;                     // an extended key
    if (!down) info |= 0xC0000000;            // it was down, and is going up
    // Alt goes as a plain key like the rest.
    //
    // Windows delivers a real Alt as WM_SYSKEYDOWN, so that is what was sent
    // at first, and the game ignored it: at a hospital bed, in front, Alt
    // answered "Вы заняли койку", and the identical press behind another
    // window answered nothing, while the window's own counter said the
    // message had arrived. The game reads WM_KEYDOWN and not its system
    // twin. Sending the plain one also keeps Windows from making a
    // SC_KEYMENU of the release, which is the thing that opens the window
    // menu and stops the game dead.
    PostMessageA(window, down ? WM_KEYDOWN : WM_KEYUP,
                 static_cast<WPARAM>(vk), info);
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
  // Somebody alt-tabbing changes the way a key has to be delivered while it
  // is still held. The old press is let go of by the road it came in on and
  // pressed again by the new one, so the two never cross.
  const bool post_now = WouldPost();
  for (int held : g_held) {
    if (held < 0 || held >= 256) continue;
    if (std::find(keys.begin(), keys.end(), held) == keys.end()) continue;
    if (g_by_message[held].load() == post_now) continue;
    SendKey(held, false);
    SendKey(held, true);
    g_events.fetch_add(2, std::memory_order_relaxed);
  }
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
    // And through the system as well, whatever the key's own route was.
    // This runs when things have already gone wrong, and a key left down at
    // the system's level is the failure that follows the person out of the
    // game and into everything else they use.
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(vk);
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(vk),
                                                   MAPVK_VK_TO_VSC));
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
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
    // Letters go at a hand's pace, a quarter of a second apart. A whole
    // sentence appearing between two frames is not something a keyboard can
    // do, and everything typed here - a line of chat, an answer in a dialog -
    // is meant to have been typed by somebody. Keys that are not letters are
    // left alone: a step in a walk is not a keystroke anybody watches.
    if (g_script[g_at].kind == Kind::kCharacter) {
      const unsigned long long now = GetTickCount64();
      if (now - g_last_letter_ms < kBetweenLettersMs) return;   // not yet
      g_last_letter_ms = now;
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
