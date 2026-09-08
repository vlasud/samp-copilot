#pragma once
//
// What the server is asking, if it is asking anything.
//
// SA-MP keeps one dialog object, pointed to by samp.dll+0x21A0B8, and shows
// it whenever the server sends one. The layout below is read off
// CDialog::Show (samp.dll+0x6B9C0) in this build: it stores the id and the
// style it was given, copies the caption into the object and keeps the text
// on the heap.
//
// Reading it matters for two reasons. A player at a dialog cannot move, so
// the walker holds still. And an agent driving this module from outside has
// no eyes: without this it cannot tell "the server is asking for a password"
// from "the game has not finished connecting".
//
#include <string>

namespace gtabot::samp {

struct Dialog {
  bool valid = false;   // the client and its dialog object were found
  bool shown = false;
  int  id    = -1;
  // 0 message box, 1 input, 2 list, 3 password input, 4 tab list,
  // 5 tab list with headers.
  int  style = -1;
  std::string caption;
  std::string text;
};

// Game thread.
Dialog CurrentDialog();

// "message box", "password input", and so on.
const char* DialogStyleName(int style);

}  // namespace gtabot::samp
