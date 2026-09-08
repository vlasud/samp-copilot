#pragma once
//
// What the server writes on the screen.
//
// The other half of what a player is told and a reader of the chat never
// sees: the money in the corner, the hunger bar, the name of the zone, the
// prompt that says which key opens the thing in front of him. The server
// draws all of it with text draws, and a great many servers put their whole
// interface there.
//
// Two kinds share one pool: the ones the server shows everybody, and the
// two hundred and fifty-six it can address to this player alone. Both are
// read here; which is which is in `for_me`.
//
#include <cstdint>
#include <string>
#include <vector>

namespace gtabot::samp {

struct TextDraw {
  int   id = -1;
  bool  for_me = false;    // one of the player's own rather than everybody's
  std::string text;
  float x = 0, y = 0;      // where on the screen the server put it
  std::uint32_t letter_colour = 0;
  int   model = 0;         // when it shows a thing rather than words
};

// Everything the client is holding. Reading only, so any thread will do.
std::vector<TextDraw> TextDraws(std::size_t max);

// For saying whether the pool was reachable at all.
std::string TextDrawsNote();

}  // namespace gtabot::samp
