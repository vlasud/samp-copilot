#pragma once
//
// The words a server puts over somebody's head.
//
// `SetPlayerChatBubble` is how a server makes an NPC answer, and how a good
// many of them make players speak at all: the text appears above the head for
// a few seconds and never reaches the chat log. A character who reads only
// the log is deaf to it.
//
// Where the client keeps that text is not in any structure this project has a
// header for - not the player pool, not the remote player, not the player
// info - so it is not guessed at. It is found the way the chat log was found:
// by looking for text that is on screen right now.
//
// `FindText` is the tool for that, and it is a diagnostic, not a feature: give
// it a string that is over somebody's head at this moment and it says where in
// the client that string lives. Once the same offset from the same base holds
// the bubble twice running, it stops being a guess and can be read directly.
//
#include <cstdint>
#include <string>
#include <vector>

namespace gtabot::samp {

struct Found {
  std::uintptr_t at = 0;
  std::string    where;      // "samp.dll+0x1234", "heap", "gta_sa.exe+..."
  std::string    around;     // a little of what is on either side
  std::string    encoding;   // which encoding of the query matched here
};

// Every place the client holds this text. `max` caps how many are reported;
// the search itself is bounded and safe on a running game.
std::vector<Found> FindText(const std::string& text, std::size_t max);

std::string FindTextNote();

}  // namespace gtabot::samp
