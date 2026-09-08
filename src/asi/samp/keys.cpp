#include "samp/keys.hpp"

#include <windows.h>

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

void Wipe() {
  if (!g_script.empty())
    SecureZeroMemory(g_script.data(), g_script.size() * sizeof(Step));
  g_script.clear();
  g_at = 0;
}

// A character, whatever the keyboard layout is set to: the scan code field
// carries the code point itself and Windows delivers it as typed.
void SendCharacter(wchar_t ch) {
  INPUT in[2] = {};
  in[0].type = INPUT_KEYBOARD;
  in[0].ki.wScan = static_cast<WORD>(ch);
  in[0].ki.dwFlags = KEYEVENTF_UNICODE;
  in[1] = in[0];
  in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
  SendInput(2, in, sizeof(INPUT));
}

void SendKey(int vk, bool down) {
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
