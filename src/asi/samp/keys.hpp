#pragma once
//
// A short script of keystrokes, played the way a hand plays them.
//
// Everything this module does to a dialog it does through the system's own
// input: the characters of a password, the arrow presses that move a list
// selection, the Enter that answers. One keystroke a frame, because a
// burst of thirty in a millisecond is not something a hand produces and not
// something the game is built to read.
//
// The script is wiped when it finishes. A password passes through here.
//
#include <string>

namespace gtabot::samp {

// Queue. Text is UTF-8 and goes out as characters, whatever the layout.
void KeysType(const std::string& utf8);
void KeysTypeWide(const std::wstring& text);
void KeysPress(int virtual_key, int times = 1);

// Game thread, once a frame.
void KeysTick();

bool KeysBusy();
void KeysClear();

}  // namespace gtabot::samp
