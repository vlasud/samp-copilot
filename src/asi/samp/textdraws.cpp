#include "samp/textdraws.hpp"

#include <windows.h>

#include <cstring>

#include "samp/version.hpp"
#include "state/memory.hpp"
#include "types.hpp"

namespace gtabot::samp {
namespace {

// From the client's own structures (github.com/BlastHackNet/SAMP-API,
// 0.3.7-R1), packed like everything else there.
//
//   CNetGame + 0x3CD is the pools, and the text draws are at +0x10 of those.
//
//   CTextDrawPool: a flag per slot for two thousand and forty-eight the
//   server shows everybody and two hundred and fifty-six it addresses to
//   this player, then a pointer per slot to the text draw itself.
//
//   CTextDraw: the text at the front, eight hundred and one bytes of it,
//   then the string the client builds from it, then the data. Inside the
//   data the position is at +0x28 and +0x2C - which puts it at +0x98B in
//   the object, and the field the structures call field_99B lands exactly
//   where the sum says it should, which is what says the sum is right.
constexpr std::uint32_t kNetGameRva  = 0x21A0F8;
constexpr std::uint32_t kPoolsAt     = 0x3CD;
constexpr std::uint32_t kTextDrawsAt = 0x10;

constexpr int kGlobal = 2048;
constexpr int kLocal  = 256;
constexpr int kSlots  = kGlobal + kLocal;
constexpr std::uint32_t kPointersAt = kSlots * 4;      // after the flags

constexpr std::uint32_t kText  = 0x000;                // char[801]
constexpr std::size_t   kTextBytes = 801;
constexpr std::uint32_t kData  = 0x963;
constexpr std::uint32_t kLetterColour = kData + 0x08;
constexpr std::uint32_t kX     = kData + 0x28;
constexpr std::uint32_t kY     = kData + 0x2C;
constexpr std::uint32_t kModel = kData + 0x45;

std::string g_note = "not looked at yet";

std::uintptr_t Pool() {
  const Client client = Detect();
  if (client.base == 0) return 0;
  std::uint32_t netgame = 0;
  if (!asi::mem::Read<std::uint32_t>(client.base + kNetGameRva, &netgame) ||
      netgame == 0)
    return 0;
  std::uint32_t pools = 0;
  if (!asi::mem::Read<std::uint32_t>(netgame + kPoolsAt, &pools) || pools == 0)
    return 0;
  std::uint32_t draws = 0;
  if (!asi::mem::Read<std::uint32_t>(pools + kTextDrawsAt, &draws) || draws == 0)
    return 0;
  if (!asi::mem::IsReadable(draws, kPointersAt + kSlots * 4)) return 0;
  return draws;
}

}  // namespace

std::vector<TextDraw> TextDraws(std::size_t max) {
  std::vector<TextDraw> found;
  const std::uintptr_t pool = Pool();
  if (pool == 0) {
    g_note = "the client's text draw pool is not where this build keeps it";
    return found;
  }
  int in_use = 0;
  for (int i = 0; i < kSlots; ++i) {
    std::uint32_t used = 0;
    if (!asi::mem::Read<std::uint32_t>(pool + i * 4, &used) || used == 0) continue;
    std::uint32_t object = 0;
    if (!asi::mem::Read<std::uint32_t>(pool + kPointersAt + i * 4, &object) ||
        object == 0)
      continue;
    ++in_use;
    if (found.size() >= max) continue;

    char text[kTextBytes + 1] = {};
    const std::size_t got = asi::mem::ReadGuarded(object + kText, text, kTextBytes);
    if (got == 0) continue;

    TextDraw draw;
    draw.id = i;
    draw.for_me = i >= kGlobal;
    draw.text = ToUtf8(std::string(text, ::strnlen(text, got)));
    asi::mem::Read<float>(object + kX, &draw.x);
    asi::mem::Read<float>(object + kY, &draw.y);
    asi::mem::Read<std::uint32_t>(object + kLetterColour, &draw.letter_colour);
    std::uint16_t model = 0;
    if (asi::mem::Read<std::uint16_t>(object + kModel, &model)) draw.model = model;
    if (draw.text.empty() && draw.model == 0) continue;
    found.push_back(std::move(draw));
  }
  g_note = std::to_string(in_use) + " of the client's " + std::to_string(kSlots) +
           " text draw slots are in use";
  return found;
}

std::string TextDrawsNote() { return g_note; }

}  // namespace gtabot::samp
