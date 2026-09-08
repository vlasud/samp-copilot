#pragma once
//
// Lets go of the mouse while the panel wants it.
//
// GTA holds the pointer in the middle of the screen: it puts it back there
// every frame and reads how far it had moved since, which is how mouse-look
// works. That leaves nothing for a panel to click with - ImGui asks Windows
// where the cursor is, and Windows truthfully answers "the middle", again and
// again.
//
// So while the panel is interactive, the two calls that implement that trick
// are answered rather than performed: the game's attempt to move the pointer
// is swallowed, and its attempt to read the position is told the value it
// last asked for. The game therefore sees a cursor that never moves and keeps
// the camera still, the real pointer moves with the hand, and the panel reads
// the real one through the untouched original.
//
// Two things this deliberately is not. It is not a patch to the game's code -
// nothing is written to gta_sa.exe or samp.dll, only two of our own process's
// calls into user32 are answered differently. And it is not permanent: the
// moment the panel stops being interactive both calls go straight through
// again, and unloading the module removes them entirely.
//
#include <windows.h>

#include <cstdint>

namespace gtabot::asi {

class CursorHook {
 public:
  // MinHook must already be initialised; the frame hook does that.
  static bool Install();
  static void Uninstall();

  // True while the pointer belongs to the panel rather than to the game.
  static void SetFreed(bool freed);
  static bool freed();

  // The real cursor, read through the original call rather than our answer to
  // it. This is what the panel positions itself with.
  static bool RealCursorPos(POINT* out);

  static bool installed();

  // Where the pointer was last put by anybody's SetCursorPos, and when. A
  // WM_MOUSEMOVE that lands exactly there was made by that call, not by
  // the hand.
  static bool LastSetTarget(POINT* out, unsigned long long* when_ms);
  // How many times the game has been told "yes" without the pointer moving.
  // Zero while the panel is interactive means the pointer is being held still
  // by something else, and the log says what.
  static std::uint64_t suppressed();
};

}  // namespace gtabot::asi
