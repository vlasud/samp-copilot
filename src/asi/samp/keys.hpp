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
#include <vector>

namespace gtabot::samp {

// Queue. Text is UTF-8 and goes out as characters, whatever the layout.
void KeysType(const std::string& utf8);
void KeysTypeWide(const std::wstring& text);
void KeysPress(int virtual_key, int times = 1);
// Held down for this many frames, then let go: a horn that has to sound, a
// throttle that has to be felt.
void KeysPressFor(int virtual_key, int frames);

// Holds exactly these keys and no others: what is held and not wanted is
// let go, what is wanted and not held goes down. The walk and the drive
// both steer this way, and only one of them runs at a time.
void KeysHold(const std::vector<int>& keys);
void KeysReleaseAll();

// How many key events have gone out, for the frame record.
unsigned long long KeysEventsSent();

// Game thread, once a frame.
void KeysTick();

bool KeysBusy();
void KeysClear();

}  // namespace gtabot::samp
