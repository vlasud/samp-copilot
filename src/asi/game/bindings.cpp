#include "game/bindings.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "game/exe.hpp"
#include "state/memory.hpp"

namespace gtabot::game {
namespace {

// CControllerConfigManager: an array of actions, each 0x20 bytes, each
// holding a primary key and an alternative eight bytes apart.
constexpr std::uint32_t kControlsManager = 0xB70198;
constexpr std::uint32_t kActions    = 0xB70;
constexpr std::uint32_t kActionSize = 0x20;
constexpr std::uint32_t kKeySize    = 0x08;

// The game's own codes for the keys that have no ASCII of their own.
constexpr int kRsUp = 1019, kRsDown = 1020, kRsLeft = 1021, kRsRight = 1022;
constexpr int kRsBackspace = 1042, kRsTab = 1043, kRsEnter = 1045;
constexpr int kRsLShift = 1046, kRsRShift = 1047, kRsShift = 1048;
constexpr int kRsLCtrl = 1049, kRsRCtrl = 1050, kRsLAlt = 1051, kRsRAlt = 1052;

int VirtualKey(int code) {
  if (code > 0 && code < 256) return code;
  switch (code) {
    case kRsUp:        return VK_UP;
    case kRsDown:      return VK_DOWN;
    case kRsLeft:      return VK_LEFT;
    case kRsRight:     return VK_RIGHT;
    case kRsEnter:     return VK_RETURN;
    case kRsLShift:
    case kRsShift:     return VK_LSHIFT;
    case kRsRShift:    return VK_RSHIFT;
    case kRsLCtrl:     return VK_LCONTROL;
    case kRsRCtrl:     return VK_RCONTROL;
    case kRsLAlt:      return VK_LMENU;
    case kRsRAlt:      return VK_RMENU;
    case kRsTab:       return VK_TAB;
    case kRsBackspace: return VK_BACK;
    default:           return 0;
  }
}

}  // namespace

int KeyForAction(int action, int fallback) {
  const std::uintptr_t table = At(kControlsManager);
  if (table == 0) return fallback;
  const std::uintptr_t entry = table + kActions +
                               static_cast<std::uint32_t>(action) * kActionSize;
  // The alternative first: on a default setup the arrows are primary and
  // WASD the alternative, and WASD is what a person actually walks on.
  for (int type = 1; type >= 0; --type) {
    std::uint32_t code = 0;
    if (!asi::mem::Read<std::uint32_t>(entry + type * kKeySize, &code)) continue;
    const int vk = VirtualKey(static_cast<int>(code));
    if (vk != 0) return vk;
  }
  return fallback;
}

std::string KeyName(int virtual_key) {
  if (virtual_key >= '0' && virtual_key <= 'Z')
    return std::string(1, static_cast<char>(virtual_key));
  switch (virtual_key) {
    case VK_SPACE:    return "Space";
    case VK_LSHIFT:   return "LShift";
    case VK_RSHIFT:   return "RShift";
    case VK_LCONTROL: return "LCtrl";
    case VK_RCONTROL: return "RCtrl";
    case VK_LMENU:    return "LAlt";
    case VK_RMENU:    return "RAlt";
    case VK_UP:       return "Up";
    case VK_DOWN:     return "Down";
    case VK_LEFT:     return "Left";
    case VK_RIGHT:    return "Right";
    case VK_RETURN:   return "Enter";
    case VK_TAB:      return "Tab";
    case VK_BACK:     return "Backspace";
    default: {
      char text[16];
      std::snprintf(text, sizeof(text), "vk%02X", virtual_key);
      return text;
    }
  }
}

}  // namespace gtabot::game
