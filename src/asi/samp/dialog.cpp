#include "samp/dialog.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "log.hpp"
#include "samp/version.hpp"
#include "state/memory.hpp"

namespace gtabot::samp {
namespace {

// samp.dll 0.3.7-R1. The offsets come from CDialog::Show at +0x6B9C0, which
// writes the style to +0x2C and the id to +0x30, frees and replaces the text
// pointer at +0x34, and copies the caption into the sixty-four bytes at
// +0x40. The dword at +0x28 is what it tests before hiding, so it is the
// flag that says one is on screen.
constexpr std::uint32_t kDialogRva = 0x21A0B8;
constexpr std::uint32_t kShown   = 0x28;
constexpr std::uint32_t kStyle   = 0x2C;
constexpr std::uint32_t kId      = 0x30;
constexpr std::uint32_t kText    = 0x34;
constexpr std::uint32_t kCaption = 0x40;
constexpr std::size_t   kCaptionBytes = 64;
constexpr std::size_t   kMaxText = 2048;

std::string ReadString(std::uintptr_t at, std::size_t limit) {
  if (at == 0) return {};
  std::string out;
  char buffer[256];
  while (out.size() < limit) {
    const std::size_t want = limit - out.size() < sizeof(buffer)
                                 ? limit - out.size()
                                 : sizeof(buffer);
    const std::size_t got = asi::mem::ReadGuarded(at + out.size(), buffer, want);
    if (got == 0) break;
    const std::size_t end = ::strnlen(buffer, got);
    out.append(buffer, end);
    if (end < got) break;   // the terminator was inside what was read
  }
  return out;
}

}  // namespace

const char* DialogStyleName(int style) {
  switch (style) {
    case 0:  return "message box";
    case 1:  return "input";
    case 2:  return "list";
    case 3:  return "password input";
    case 4:  return "tab list";
    case 5:  return "tab list with headers";
    default: return "unknown";
  }
}

Dialog CurrentDialog() {
  Dialog out;
  const Client client = Detect();
  if (client.base == 0) return out;
  std::uint32_t object = 0;
  if (!asi::mem::Read<std::uint32_t>(client.base + kDialogRva, &object) || object == 0)
    return out;
  if (!asi::mem::IsReadable(object, kCaption + kCaptionBytes)) return out;
  out.valid = true;

  std::uint32_t shown = 0;
  asi::mem::Read<std::uint32_t>(object + kShown, &shown);
  out.shown = shown != 0;
  asi::mem::Read<int>(object + kStyle, &out.style);
  asi::mem::Read<int>(object + kId, &out.id);
  out.caption = ReadString(object + kCaption, kCaptionBytes);
  std::uint32_t text = 0;
  if (asi::mem::Read<std::uint32_t>(object + kText, &text) && text != 0)
    out.text = ReadString(text, kMaxText);
  return out;
}

}  // namespace gtabot::samp
