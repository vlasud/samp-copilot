#include "game/input_probe.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "game/exe.hpp"
#include "game/pad_watch.hpp"
#include "state/memory.hpp"

namespace gtabot::game {
namespace {

using asi::mem::Read;

// gta_sa.exe 1.0 US. Keyboard state the window procedure writes into, and
// the two copies UpdatePads rotates it through. CKeyboardState is 0x270
// bytes: FKeys[12] first, then standardKeys[256], then the named keys.
constexpr std::uint32_t kTempKeyState = 0xB72CB0;
constexpr std::uint32_t kOldKeyState  = 0xB72F20;
constexpr std::uint32_t kNewKeyState  = 0xB73190;
constexpr std::uint32_t kStandardKeys = 0x18;
constexpr std::uint32_t kEscKey       = 0x18 + 0x200;

// CPad::Pads[0] and the members that matter here.
constexpr std::uint32_t kPad0            = 0xB73458;
constexpr std::uint32_t kLeftStickX      = 0x000;
constexpr std::uint32_t kLeftStickY      = 0x002;
constexpr std::uint32_t kPadPhase        = 0x108;
constexpr std::uint32_t kPadMode         = 0x10A;
constexpr std::uint32_t kDisableControls = 0x10E;
// CPad::padNumber: nonzero sends the keyboard to the second pad instead.
constexpr std::uint32_t kPadNumber = 0xB73400;
// ControlsManager and its action table: 0x20 per action, four 8-byte keys
// per action (keyboard, extra keyboard, mouse, joystick), key code first.
constexpr std::uint32_t kControlsManager = 0xB70198;
constexpr std::uint32_t kActions         = 0xB70;
constexpr std::uint32_t kActionSize      = 0x20;
constexpr std::uint32_t kKeySize         = 0x8;
constexpr int kGoForward = 4, kGoBack = 5, kGoLeft = 6, kGoRight = 7;
// The "both directions down" flags the conversion sets when opposite keys
// cancel: four per stick axis, one per controller type, keyboard first.
constexpr std::uint32_t kBothDown = 0x12D0;
// Whether the camera follows the mouse at all, and the mouse as the game
// last read it through DirectInput: the deltas, so a dead mouse and a
// locked camera can be told apart.
constexpr std::uint32_t kUseMouse3rdPerson = 0xB6EC2E;
constexpr std::uint32_t kNewMouseState     = 0xB73418;
constexpr std::uint32_t kMouseX            = 0x8;
constexpr std::uint32_t kMouseY            = 0xC;
// TheCamera: which cam is active, and that cam's mode.
constexpr std::uint32_t kCamera     = 0xB6F028;
constexpr std::uint32_t kActiveCam  = 0x59;
constexpr std::uint32_t kCams       = 0x174;
constexpr std::uint32_t kCamSize    = 0x238;
constexpr std::uint32_t kCamMode    = 0xC;
// The named keys after standardKeys, the ones a movement key can be
// cancelled by without ever showing in the standard set.
struct NamedKey { std::uint32_t offset; const char* name; };
constexpr NamedKey kNamedKeys[] = {
    {0x218 + 7 * 2, "up"},      {0x218 + 8 * 2, "down"},   {0x218 + 9 * 2, "left"},
    {0x218 + 10 * 2, "right"},  {0x218 + 31 * 2, "tab"},   {0x218 + 34 * 2, "lshift"},
    {0x218 + 35 * 2, "rshift"}, {0x218 + 37 * 2, "lctrl"}, {0x218 + 38 * 2, "rctrl"},
    {0x218 + 39 * 2, "lalt"},   {0x218 + 40 * 2, "ralt"},
};

// Set by WM_ACTIVATE and WM_SETFOCUS, cleared by WM_ACTIVATE(inactive) and
// WM_KILLFOCUS, and the first thing CPad::UpdateMouse checks.
constexpr std::uint32_t kForegroundApp = 0x8D621C;
// FrontEndMenuManager.m_bMenuActive, and the flag WM_SETFOCUS raises to open
// the menu on the next frame.
constexpr std::uint32_t kMenuActive    = 0xBA67A4;
constexpr std::uint32_t kMenuNextFrame = 0xBA677B;
// CTimer.
constexpr std::uint32_t kCodePause = 0xB7CB48;
constexpr std::uint32_t kUserPause = 0xB7CB49;
constexpr std::uint32_t kTimeStep  = 0xB7CB5C;
// CWorld.
constexpr std::uint32_t kCutsceneOnly  = 0xB7CD6D;
constexpr std::uint32_t kPlayerInFocus = 0xB7CD74;
constexpr std::uint32_t kScanCode      = 0xB7CD78;
constexpr std::uint32_t kPlayers       = 0xB7CD98;   // CPlayerInfo[2], m_pPed first

// CEntity / CPhysical / CPed, from the plugin-sdk declarations for 1.0 US.
constexpr std::uint32_t kEntityFlagsA   = 0x1C;
constexpr std::uint32_t kEntityFlagsB   = 0x20;
constexpr std::uint32_t kPhysicalFlags  = 0x40;
constexpr std::uint32_t kMoveSpeed      = 0x44;
constexpr std::uint32_t kAttachedTo     = 0xFC;
constexpr std::uint32_t kPedFlagsA      = 0x474;
constexpr std::uint32_t kPedFlagsB      = 0x478;
constexpr std::uint32_t kIntelligence   = 0x47C;
constexpr std::uint32_t kPedState       = 0x530;
constexpr std::uint32_t kMoveState      = 0x534;
constexpr std::uint32_t kHealth         = 0x540;
constexpr std::uint32_t kVehicle        = 0x58C;
constexpr std::uint32_t kPedType        = 0x598;
// CPedIntelligence::m_TaskMgr, and inside it the two task arrays.
constexpr std::uint32_t kTaskManager    = 0x4;
constexpr std::uint32_t kPrimaryTasks   = 0x0;
constexpr std::uint32_t kSecondaryTasks = 0x14;
// CTask::GetId is the fifth virtual, and in this build it is six bytes:
// mov eax, imm32; ret.
constexpr std::uint32_t kGetIdSlot = 0x10;

template <typename T>
T ReadOr(std::uintptr_t at, T fallback) {
  T value{};
  return Read<T>(at, &value) ? value : fallback;
}

// Which keys a CKeyboardState says are down. Letters and digits are shown as
// themselves, so a key that lands in the wrong slot - the layout-dependent
// translation the game does - is visible as a strange character.
std::string HeldKeys(std::uintptr_t state) {
  // One copy of the whole block, not a validated read per key.
  std::int16_t keys[256] = {};
  if (asi::mem::ReadGuarded(state + kStandardKeys, keys, sizeof(keys)) !=
      sizeof(keys))
    return "?";
  std::string out;
  int shown = 0;
  for (int code = 0; code < 256 && shown < 8; ++code) {
    if (keys[code] == 0) continue;
    char buffer[8];
    if (code >= 0x21 && code < 0x7F)
      std::snprintf(buffer, sizeof(buffer), "%c", static_cast<char>(code));
    else
      std::snprintf(buffer, sizeof(buffer), "%02X", code);
    if (!out.empty()) out += ",";
    out += buffer;
    ++shown;
  }
  if (ReadOr<std::int16_t>(state + kEscKey, 0) != 0) {
    if (!out.empty()) out += ",";
    out += "esc";
  }
  for (const NamedKey& named : kNamedKeys) {
    if (ReadOr<std::int16_t>(state + named.offset, 0) == 0) continue;
    if (!out.empty()) out += ",";
    out += named.name;
  }
  return out.empty() ? "-" : out;
}

std::string WindowName(HWND window) {
  if (window == nullptr) return "NONE";
  char cls[64] = "";
  GetClassNameA(window, cls, sizeof(cls));
  DWORD pid = 0;
  const DWORD tid = GetWindowThreadProcessId(window, &pid);
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "0x%08X(%s tid=%lu%s)",
                static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(window)),
                cls, static_cast<unsigned long>(tid),
                pid == GetCurrentProcessId() ? "" : " OTHER PROCESS");
  return buffer;
}

// The game thread's own view of who has its keyboard. GetFocus() is per
// thread and useless from the worker; this asks the right thread.
std::string FocusPicture(HWND game_window, bool full) {
  if (game_window == nullptr) return " kbfocus=?";
  const DWORD tid = GetWindowThreadProcessId(game_window, nullptr);
  GUITHREADINFO info{};
  info.cbSize = sizeof(info);
  if (!GetGUIThreadInfo(tid, &info)) return " kbfocus=?";
  std::string out;
  if (info.hwndFocus == game_window)
    out += " kbfocus=game";
  else
    out += " kbfocus=" + WindowName(info.hwndFocus);
  if (full || info.hwndActive != game_window)
    out += " active=" + (info.hwndActive == game_window ? std::string("game")
                                                        : WindowName(info.hwndActive));
  if (info.hwndCapture != nullptr)
    out += " capture=" + (info.hwndCapture == game_window ? std::string("game")
                                                          : WindowName(info.hwndCapture));
  char layout[32];
  std::snprintf(layout, sizeof(layout), " layout=%04X",
                static_cast<unsigned>(LOWORD(reinterpret_cast<std::uintptr_t>(
                    GetKeyboardLayout(tid)))));
  out += layout;
  return out;
}

// What CPad::Update saw and did last frame, from the hook on it.
std::string ReconcilePicture(bool full) {
  const game::PadPicture p = LastPadPicture();
  if (!p.seen) return " upd=?";
  char buffer[200];
  std::snprintf(buffer, sizeof(buffer), " key=%d,%d joy=%d,%d new=%d,%d%s",
                p.key_x, p.key_y, p.joy_x, p.joy_y, p.new_x, p.new_y,
                p.quieted ? "(joy silenced)" : "");
  std::string out = buffer;
  if (p.fallback) out += "(kb fallback)";
  // The gates of the game's own keyboard-to-pad conversion, always: they
  // are what decides whether key becomes stick.
  const std::uintptr_t ped = ReadOr<std::uint32_t>(At(kPlayers), 0);
  const std::uint32_t flags = ped ? ReadOr<std::uint32_t>(ped + kPedFlagsA, 0) : 0;
  const std::uint8_t cam = ReadOr<std::uint8_t>(At(kCamera) + kActiveCam, 0xFF);
  const int mode = cam < 3 ? ReadOr<int>(At(kCamera) + kCams + cam * kCamSize + kCamMode, -1) : -1;
  const std::uintptr_t both = At(kControlsManager) + kBothDown;
  // The mouse: when the game last saw it move. Bucketed, so a hand on the
  // mouse does not write a line a quarter-second.
  static unsigned long long last_mouse_ms = 0;
  const float mx = ReadOr<float>(At(kNewMouseState) + kMouseX, 0.0f);
  const float my = ReadOr<float>(At(kNewMouseState) + kMouseY, 0.0f);
  const unsigned long long now = GetTickCount64();
  if (mx != 0.0f || my != 0.0f) last_mouse_ms = now;
  const unsigned long long mouse_age = last_mouse_ms ? (now - last_mouse_ms) / 1000 : 999;
  const char* mouse = mouse_age < 2 ? "live" : mouse_age < 10 ? "quiet" : "silent";
  std::snprintf(buffer, sizeof(buffer),
                " pn=%d st=%d inv=%d cam=%d both=%d%d%d%d m3p=%d mouse=%s kbfb=%llu",
                ReadOr<std::uint8_t>(At(kPadNumber), 0xFF),
                ped ? ReadOr<int>(ped + kPedState, -1) : -1, (flags >> 8) & 1, mode,
                ReadOr<std::uint8_t>(both + 0, 9), ReadOr<std::uint8_t>(both + 1, 9),
                ReadOr<std::uint8_t>(both + 4, 9), ReadOr<std::uint8_t>(both + 5, 9),
                ReadOr<std::uint8_t>(At(kUseMouse3rdPerson), 0xFF), mouse,
                KeyboardFallbackFrames());
  out += buffer;
  if (!full) return out;
  std::snprintf(buffer, sizeof(buffer),
                " keydpad=%d joyr=%d,%d joydpad=%d joybtn=%d mouse=%d,%d "
                "joyspoke=%llu",
                p.key_dpad, p.joy_rx, p.joy_ry, p.joy_dpad, p.joy_buttons,
                p.mouse_x, p.mouse_y, GamepadSpokeFrames());
  out += buffer;
  return out;
}

std::string Bindings() {
  const std::uintptr_t table = At(kControlsManager) + kActions;
  char buffer[160];
  const auto key = [&](int action, int type) {
    return ReadOr<std::uint32_t>(table + action * kActionSize + type * kKeySize, 0xFFFF);
  };
  std::snprintf(buffer, sizeof(buffer),
                " pn=%d bind(fwd,back,left,right)=%X/%X %X/%X %X/%X %X/%X",
                ReadOr<std::uint8_t>(At(kPadNumber), 0xFF),
                key(kGoForward, 0), key(kGoForward, 1), key(kGoBack, 0),
                key(kGoBack, 1), key(kGoLeft, 0), key(kGoLeft, 1),
                key(kGoRight, 0), key(kGoRight, 1));
  return buffer;
}

std::string PadStatePicture(bool full) {
  char buffer[256];
  const std::uintptr_t pad = At(kPad0);
  if (pad == 0) return " pad=?";
  std::snprintf(buffer, sizeof(buffer),
                " fg_app=%d tmp=[%s] new=[%s] stick=%d,%d menu=%d pause=%d/%d",
                ReadOr<std::int32_t>(At(kForegroundApp), -1),
                HeldKeys(At(kTempKeyState)).c_str(),
                HeldKeys(At(kNewKeyState)).c_str(),
                ReadOr<std::int16_t>(pad + kLeftStickX, 0),
                ReadOr<std::int16_t>(pad + kLeftStickY, 0),
                ReadOr<std::uint8_t>(At(kMenuActive), 0xFF),
                ReadOr<std::uint8_t>(At(kUserPause), 0xFF),
                ReadOr<std::uint8_t>(At(kCodePause), 0xFF));
  std::string out = buffer;
  out += ReconcilePicture(full);
  if (!full) return out;
  out += Bindings();
  std::snprintf(buffer, sizeof(buffer),
                " old=[%s] dpc=0x%X mode=%d phase=%d menu_next=%d timestep=%.3f "
                "cut_only=%d player_in_focus=%d players0_ped=0x%08X scan=%u",
                HeldKeys(At(kOldKeyState)).c_str(),
                ReadOr<std::uint16_t>(pad + kDisableControls, 0xFFFF),
                ReadOr<std::int16_t>(pad + kPadMode, -1),
                ReadOr<std::uint8_t>(pad + kPadPhase, 0xFF),
                ReadOr<std::uint8_t>(At(kMenuNextFrame), 0xFF),
                ReadOr<float>(At(kTimeStep), -1.0f),
                ReadOr<std::uint8_t>(At(kCutsceneOnly), 0xFF),
                ReadOr<std::uint8_t>(At(kPlayerInFocus), 0xFF),
                ReadOr<std::uint32_t>(At(kPlayers), 0),
                ReadOr<std::uint16_t>(At(kScanCode), 0));
  out += buffer;
  return out;
}

// A task's type, read off its GetId without calling it: the function is a
// six-byte "mov eax, imm32; ret" and the immediate is the type. Anything
// else - a relocated stub, a freed task - reads as a negative number.
int TaskType(std::uintptr_t task) {
  std::uint32_t vtable = 0;
  if (!Read<std::uint32_t>(task, &vtable) || vtable == 0) return -1;
  std::uint32_t get_id = 0;
  if (!Read<std::uint32_t>(vtable + kGetIdSlot, &get_id) || get_id == 0) return -1;
  unsigned char code[6];
  if (asi::mem::ReadGuarded(get_id, code, sizeof(code)) != sizeof(code)) return -1;
  if (code[0] != 0xB8 || code[5] != 0xC3) return -2;
  std::int32_t type = 0;
  std::memcpy(&type, code + 1, 4);
  return type;
}

std::string TaskList(std::uintptr_t array, int count) {
  std::string out;
  for (int i = 0; i < count; ++i) {
    const std::uint32_t task = ReadOr<std::uint32_t>(array + i * 4, 0);
    if (!out.empty()) out += ",";
    if (task == 0) {
      out += "-";
      continue;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%d", TaskType(task));
    out += buffer;
  }
  return out;
}

std::string PedPicture(std::uintptr_t ped) {
  if (ped == 0) return " ped=none";
  char buffer[320];
  float speed[3] = {0, 0, 0};
  Read<float>(ped + kMoveSpeed + 0, &speed[0]);
  Read<float>(ped + kMoveSpeed + 4, &speed[1]);
  Read<float>(ped + kMoveSpeed + 8, &speed[2]);
  std::snprintf(buffer, sizeof(buffer),
                " ped=0x%08X eflags=0x%08X,0x%08X phys=0x%08X speed=%.3f,%.3f,%.3f "
                "pedflags=0x%08X,0x%08X state=%d move=%d hp=%.0f veh=0x%08X "
                "type=%d attached=0x%08X",
                static_cast<unsigned>(ped),
                ReadOr<std::uint32_t>(ped + kEntityFlagsA, 0),
                ReadOr<std::uint32_t>(ped + kEntityFlagsB, 0),
                ReadOr<std::uint32_t>(ped + kPhysicalFlags, 0),
                speed[0], speed[1], speed[2],
                ReadOr<std::uint32_t>(ped + kPedFlagsA, 0),
                ReadOr<std::uint32_t>(ped + kPedFlagsB, 0),
                ReadOr<std::int32_t>(ped + kPedState, -1),
                ReadOr<std::int32_t>(ped + kMoveState, -1),
                ReadOr<float>(ped + kHealth, -1.0f),
                ReadOr<std::uint32_t>(ped + kVehicle, 0),
                ReadOr<std::int32_t>(ped + kPedType, -1),
                ReadOr<std::uint32_t>(ped + kAttachedTo, 0));
  std::string out = buffer;
  const std::uint32_t intelligence = ReadOr<std::uint32_t>(ped + kIntelligence, 0);
  if (intelligence != 0) {
    const std::uintptr_t tasks = intelligence + kTaskManager;
    out += " tasks=p[" + TaskList(tasks + kPrimaryTasks, 5) + "] s[" +
           TaskList(tasks + kSecondaryTasks, 6) + "]";
  } else {
    out += " tasks=?";
  }
  return out;
}

struct Site {
  std::uint32_t address;
  const char*   name;
  // First eight bytes on disk, or none when only the jump target matters.
  const unsigned char* expected;
};

constexpr unsigned char kFindGround[]   = {0x83, 0xEC, 0x38, 0x8B, 0x44, 0x24, 0x3C, 0x8B};
constexpr unsigned char kLineOfSight[]  = {0x83, 0xEC, 0x5C, 0x66, 0x81, 0x3D, 0x78, 0xCD};
constexpr unsigned char kScreenCoors[]  = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x0C, 0x56};
constexpr unsigned char kVerticalLine[] = {0x83, 0xEC, 0x2C, 0x90, 0x90, 0x90, 0x90, 0xE9};
constexpr unsigned char kClearScan[]    = {0xE9, 0xE3, 0x4D, 0xEA, 0xFF, 0x33, 0xD2, 0x56};
constexpr unsigned char kMainWndProc[]  = {0x83, 0xEC, 0x3C, 0x53, 0x55, 0x8B, 0x6C, 0x24};
constexpr unsigned char kAffectKeys[]   = {0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57, 0x8D};
constexpr unsigned char kPadUpdate[]    = {0x83, 0xEC, 0x30, 0x53, 0x55, 0x8B, 0xD9, 0x56};
constexpr unsigned char kKeyHandler[]   = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xE8, 0x1C, 0x74};
constexpr unsigned char kTranslateKey[] = {0x8B, 0x4C, 0x24, 0x0C, 0x56, 0x8B, 0x74, 0x24};

const Site kSites[] = {
    {0x5696C0, "FindGroundZ", kFindGround},
    {0x56A490, "LineOfSight", kLineOfSight},
    {0x71DA00, "CalcScreen", kScreenCoors},
    {0x5674E0, "VerticalLine", kVerticalLine},
    {0x563470, "ClearScanCodes", kClearScan},
    {0x747EB0, "MainWndProc", kMainWndProc},
    {0x744880, "KeyboardHandler", kKeyHandler},
    {0x747820, "TranslateKey", kTranslateKey},
    {0x531140, "AffectPadFromKeyBoard", kAffectKeys},
    {0x541C40, "CPad::Update", kPadUpdate},
    {0x53F3C0, "UpdateMouse", nullptr},
    {0x541DD0, "UpdatePads", nullptr},
};

std::string Hex(const unsigned char* bytes, std::size_t n) {
  std::string out;
  char buffer[4];
  for (std::size_t i = 0; i < n; ++i) {
    std::snprintf(buffer, sizeof(buffer), "%02X", bytes[i]);
    out += buffer;
  }
  return out;
}

}  // namespace

std::string InputPipelineBrief(std::uintptr_t game_ped, void* game_window) {
  (void)game_ped;
  return FocusPicture(static_cast<HWND>(game_window), false) + PadStatePicture(false);
}

std::string InputPipelineFull(std::uintptr_t game_ped, void* game_window) {
  return FocusPicture(static_cast<HWND>(game_window), true) + PadStatePicture(true) +
         PedPicture(game_ped);
}

std::string CodeIntegrityReport() {
  if (!Detect().known) return "not the build the reference bytes are for";
  std::string out;
  const asi::mem::Module exe = asi::mem::FindModule(nullptr);
  for (const Site& site : kSites) {
    const std::uintptr_t at = At(site.address);
    unsigned char bytes[8] = {};
    if (!out.empty()) out += " ";
    out += site.name;
    out += "=";
    if (asi::mem::ReadGuarded(at, bytes, sizeof(bytes)) != sizeof(bytes)) {
      out += "unreadable";
      continue;
    }
    // A jump or call out of the executable at the entry is somebody's hook;
    // name whose.
    if (bytes[0] == 0xE9 || bytes[0] == 0xE8) {
      std::int32_t rel = 0;
      std::memcpy(&rel, bytes + 1, 4);
      const std::uintptr_t target = at + 5 + static_cast<std::intptr_t>(rel);
      if (!exe.contains(target)) {
        out += (bytes[0] == 0xE9 ? "jmp->" : "call->") +
               asi::mem::DescribeAddress(target);
        continue;
      }
    }
    if (site.expected == nullptr) {
      out += Hex(bytes, sizeof(bytes));
      continue;
    }
    out += std::memcmp(bytes, site.expected, 8) == 0
               ? "ok"
               : "CHANGED:" + Hex(bytes, sizeof(bytes));
  }
  return out;
}

int ThreadCount() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return -1;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  int count = 0;
  const DWORD pid = GetCurrentProcessId();
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID == pid) ++count;
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return count;
}

}  // namespace gtabot::game
