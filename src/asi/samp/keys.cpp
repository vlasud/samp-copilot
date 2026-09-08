#include "samp/keys.hpp"

#include <windows.h>

#include <mutex>
#include <vector>

namespace gtabot::samp {
namespace {

struct Step {
  bool    character = false;
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

void SendKey(int vk) {
  INPUT in[2] = {};
  in[0].type = INPUT_KEYBOARD;
  in[0].ki.wVk = static_cast<WORD>(vk);
  in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(vk),
                                                    MAPVK_VK_TO_VSC));
  if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)
    in[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  in[1] = in[0];
  in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
  SendInput(2, in, sizeof(INPUT));
}

}  // namespace

void KeysTypeWide(const std::wstring& text) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (wchar_t ch : text) g_script.push_back(Step{true, ch, 0});
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
  for (int i = 0; i < times; ++i)
    g_script.push_back(Step{false, 0, virtual_key});
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
  if (step.character)
    SendCharacter(step.ch);
  else
    SendKey(step.vk);
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
