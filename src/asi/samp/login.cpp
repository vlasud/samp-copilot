#include "samp/login.hpp"

#include <windows.h>

#include <dpapi.h>

#include <atomic>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include "game/mouse_watch.hpp"
#include "log.hpp"
#include "samp/dialog.hpp"
#include "samp/input_state.hpp"
#include "types.hpp"

#pragma comment(lib, "crypt32.lib")

namespace gtabot::samp {
namespace {

// SA-MP has a style that masks what is typed (3) and a plain one (1).
// Servers use either for a login - this one asks in plain text - so both
// are answered, and only ever before the character has first spawned.
constexpr int kPasswordStyle = 3;
constexpr int kInputStyle = 1;
// One character a frame. Fast enough to be over in a fifth of a second, slow
// enough that nothing drops it.
constexpr int kFramesBetween = 1;

std::wstring g_password;          // wiped the moment it has been typed
std::string  g_caption_filter;    // optional: only a dialog whose caption has this
bool g_auto = true;
bool g_read = false;
bool g_have = false;

std::atomic<bool> g_sent{false};
std::string g_last_note = "nothing has been asked of it yet";

// The typing itself.
bool  g_typing = false;
std::size_t g_at = 0;
int   g_wait = 0;
bool  g_enter_next = false;

bool IsHex(const std::string& text) {
  if (text.size() < 64 || (text.size() & 1) != 0) return false;
  for (char c : text)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

// The shape PowerShell's ConvertFrom-SecureString writes: the password as
// UTF-16, sealed by Windows to the account that sealed it, in hex.
bool Unprotect(const std::string& hex, std::wstring* out) {
  std::vector<BYTE> blob(hex.size() / 2);
  for (std::size_t i = 0; i < blob.size(); ++i) {
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return c - 'A' + 10;
    };
    blob[i] = static_cast<BYTE>(digit(hex[i * 2]) * 16 + digit(hex[i * 2 + 1]));
  }
  DATA_BLOB in{static_cast<DWORD>(blob.size()), blob.data()};
  DATA_BLOB plain{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &plain))
    return false;
  out->assign(reinterpret_cast<const wchar_t*>(plain.pbData), plain.cbData / 2);
  SecureZeroMemory(plain.pbData, plain.cbData);
  LocalFree(plain.pbData);
  return true;
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  const int need = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (need <= 0) return {};
  std::wstring out(static_cast<std::size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), need);
  return out;
}

void ReadFile() {
  if (g_read) return;
  g_read = true;
  const std::string path = ModuleDirectory() + "bot.login";
  std::ifstream in(path);
  if (!in) {
    g_last_note = "there is no bot.login, so nothing is typed anywhere";
    return;
  }
  std::string line;
  std::string secret;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line.rfind("password=", 0) == 0) {
      secret = line.substr(9);
    } else if (line.rfind("caption=", 0) == 0) {
      g_caption_filter = line.substr(8);
    } else if (line.rfind("auto=", 0) == 0) {
      const std::string value = line.substr(5);
      g_auto = value == "on" || value == "1" || value == "true";
    } else if (secret.empty()) {
      secret = line;   // a file holding nothing but the password
    }
  }
  if (secret.empty()) {
    g_last_note = "bot.login holds no password";
    return;
  }
  if (IsHex(secret) && Unprotect(secret, &g_password)) {
    g_have = true;
  } else {
    g_password = Widen(secret);
    g_have = !g_password.empty();
  }
  SecureZeroMemory(secret.data(), secret.size());
  if (!g_have) {
    g_last_note = "bot.login could not be read as a password";
    return;
  }
  LOG_INFO("login: a password is kept in bot.login{}{} - it will be typed into "
           "the server's password dialog and never written anywhere",
           g_caption_filter.empty() ? "" : ", for captions containing ",
           g_caption_filter);
  g_last_note = "waiting for the server to ask";
}

void SendUnicode(wchar_t ch) {
  INPUT in[2] = {};
  in[0].type = INPUT_KEYBOARD;
  in[0].ki.wScan = static_cast<WORD>(ch);
  in[0].ki.dwFlags = KEYEVENTF_UNICODE;
  in[1] = in[0];
  in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
  SendInput(2, in, sizeof(INPUT));
}

void SendReturn() {
  INPUT in[2] = {};
  in[0].type = INPUT_KEYBOARD;
  in[0].ki.wVk = VK_RETURN;
  in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
  in[1] = in[0];
  in[1].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(2, in, sizeof(INPUT));
}

bool WindowInFront() {
  HWND window = game::GameWindow();
  return window != nullptr && GetForegroundWindow() == window;
}

// Whether this dialog is the one to answer.
bool Answerable(const Dialog& dialog, std::string* why) {
  if (!dialog.valid) {
    *why = "the client's dialog could not be read";
    return false;
  }
  if (!dialog.shown) {
    *why = "no dialog is on screen";
    return false;
  }
  if (dialog.style != kPasswordStyle && dialog.style != kInputStyle) {
    *why = std::string("the dialog on screen is a ") +
           DialogStyleName(dialog.style) + ", not something to type a password into";
    return false;
  }
  if (!g_caption_filter.empty() &&
      dialog.caption.find(g_caption_filter) == std::string::npos) {
    *why = "the dialog's caption does not contain what bot.login asks for";
    return false;
  }
  return true;
}

void StartTyping() {
  g_typing = true;
  g_at = 0;
  g_wait = 0;
  g_enter_next = false;
}

// One character a frame, then Enter, then the password is gone from memory.
void TypeOneFrame() {
  if (!g_typing) return;
  if (g_wait > 0) {
    --g_wait;
    return;
  }
  g_wait = kFramesBetween;
  if (g_enter_next) {
    SendReturn();
    g_typing = false;
    g_enter_next = false;
    g_sent.store(true);
    SecureZeroMemory(g_password.data(), g_password.size() * sizeof(wchar_t));
    g_password.clear();
    g_have = false;
    g_last_note = "the password has been typed and the dialog answered";
    LOG_INFO("login: {}", g_last_note);
    return;
  }
  if (g_at >= g_password.size()) {
    g_enter_next = true;
    return;
  }
  SendUnicode(g_password[g_at++]);
}

}  // namespace

void WatchLogin() {
  ReadFile();
  if (g_typing) {
    TypeOneFrame();
    return;
  }
  if (!g_have || !g_auto || g_sent.load()) return;

  // Before the first spawn only: later on, a password dialog belongs to
  // something else entirely.
  const InputSwitch state = ReadInputSwitch();
  if (state.player_known && state.active != 0) return;

  std::string why;
  const Dialog dialog = CurrentDialog();

  // Every dialog the server puts up before the character exists is worth a
  // line: when the password is not typed, this is what says why.
  static std::string said_about;
  if (dialog.shown) {
    const std::string about = std::to_string(dialog.id) + "/" +
                              std::to_string(dialog.style) + "/" + dialog.caption;
    if (about != said_about) {
      said_about = about;
      LOG_INFO("login: before the spawn the server is showing a {} dialog, id {}, "
               "\"{}\"", DialogStyleName(dialog.style), dialog.id, dialog.caption);
    }
  }

  if (!Answerable(dialog, &why)) return;
  if (!WindowInFront()) {
    static bool complained = false;
    if (!complained) {
      complained = true;
      LOG_WARN("login: the server is asking, but the game's window is not the one "
               "in front - keys would go to whatever is, so nothing is typed");
    }
    return;
  }

  LOG_INFO("login: answering \"{}\" from bot.login", dialog.caption);
  StartTyping();
}

std::string LoginNow() {
  ReadFile();
  if (g_typing) return "already typing it";
  if (g_sent.load()) return "the password was already sent this session";
  if (!g_have) return g_last_note;
  std::string why;
  const Dialog dialog = CurrentDialog();
  if (!Answerable(dialog, &why)) return why;
  if (!WindowInFront())
    return "the game's window is not the one in front, and keys go to whatever is";
  StartTyping();
  return "typing the password into \"" + dialog.caption + "\"";
}

bool LoginConfigured() { ReadFile(); return g_have || g_sent.load(); }
bool LoginSent() { return g_sent.load(); }

std::string LoginLine() {
  if (g_sent.load()) return " login:sent";
  if (g_typing) return " login:typing";
  ReadFile();
  return g_have ? " login:ready" : " login:none";
}

}  // namespace gtabot::samp
