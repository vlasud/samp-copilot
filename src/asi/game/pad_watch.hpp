#pragma once
//
// The keyboard has a rival. Every frame CPad::Update reconciles what the
// keyboard put in the pad with what the joystick put in it, and the rules
// are not "keyboard wins": an axis the two disagree on is zeroed, and a
// d-pad flag from either side zeroes the matching stick axis outright. A
// gamepad lying on the desk - a DualSense over USB is a DirectInput
// joystick to this game - only has to say something for W, A, S and D to
// stop working, with every key still arriving. That is what the input
// "lock" looked like from the outside: keys in the keyboard state, nothing
// in the stick, focus and controls fine.
//
// So CPad::Update is hooked for the first pad. Before it runs, the three
// states it is about to reconcile are copied out - keyboard, joystick,
// mouse - and, unless the gamepad is wanted, the joystick's is cleared.
// After it runs, the stick it produced is copied out too. The worker's
// input line prints all of it, so the chain from key to stick is visible
// in one reading.
//
#include <cstdint>

namespace gtabot::game {

struct PadPicture {
  bool  seen = false;
  unsigned long long frame = 0;
  // What the keyboard put in the pad this frame, before reconciling.
  short key_x = 0, key_y = 0;
  int   key_dpad = 0;            // bits: 1 up, 2 down, 4 left, 8 right
  // What the joystick put in it.
  short joy_x = 0, joy_y = 0, joy_rx = 0, joy_ry = 0;
  int   joy_dpad = 0;
  int   joy_buttons = 0;         // how many button fields were nonzero
  // What the mouse buttons put in it.
  short mouse_x = 0, mouse_y = 0;
  // What came out.
  short new_x = 0, new_y = 0;
  bool  quieted = false;         // the joystick's state was cleared this frame
  // The keyboard fallback wrote the stick this frame, because the game had
  // not, with keys down and nothing in the way of it.
  bool  fallback = false;
};

// Hooks CPad::Update. MinHook must be initialised and the executable known;
// safe to call every tick until it takes. Honours the walker=off switch,
// being a patch to the game's code like the walker's.
bool PadWatchInstall();

PadPicture LastPadPicture();
// Frames in which the joystick had anything to say at all.
unsigned long long GamepadSpokeFrames();

void SetIgnoreGamepad(bool ignore);
bool IgnoreGamepad();

// The keyboard fallback.
//
// Whatever it is that now and then stops the game turning W, A, S and D
// into the stick - the keys arrive, the pad stays empty, the character
// stands - the player should not be the one to pay for it while it is
// found. So when the keyboard state says a movement key is down, the game
// has just produced an empty stick, and none of the game's own reasons for
// that hold (controls disabled, menu open, second pad selected, in a
// vehicle), the stick, sprint and jump are written from the keyboard
// directly. The first time it happens, the log gets every gate's value.
unsigned long long KeyboardFallbackFrames();
void SetKeyboardFallback(bool on);
bool KeyboardFallback();

}  // namespace gtabot::game
