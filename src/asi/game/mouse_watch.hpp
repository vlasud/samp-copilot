#pragma once
//
// The mouse the game does not get.
//
// GTA reads the mouse through DirectInput: every frame CPad::UpdateMouse asks
// the device for its state, and the camera turns by the deltas that come
// back. Nothing else the character does depends on it, so when DirectInput
// goes quiet the picture is exactly the one reported: the game runs, keys
// work, the window is active, and the camera will not turn. On this Windows
// dinput8.dll is no longer the old library - it is a shim over InputHost
// (ext-ms-win-mininput-inputhost), and that path has been seen to stall.
//
// So the device is watched and, when it stalls, revived. GetDeviceState and
// Acquire on the game's mouse device are hooked to count calls, results and
// motion; the window procedure counts WM_MOUSEMOVE, which is the evidence
// that the hand is moving. A mouse that moves in the window but not in
// DirectInput for a second and a half is dead, and three things are tried in
// turn, a second and a half apart: re-acquiring the device the way alt-tab
// does, re-creating it through the game's own diMouseInit, and finally
// feeding the camera from the window messages themselves until DirectInput
// speaks again.
//
#include <windows.h>

#include <cstdint>
#include <string>

namespace gtabot::game {

// The game's window, as the game itself records it.
HWND GameWindow();

// Hooks the game's DirectInput mouse device. MinHook must be initialised and
// the device created; safe to call every tick until it takes.
bool MouseWatchInstall();

// From the window procedure: the pointer moved to (x, y) in client
// coordinates; a raw-input message arrived.
void MouseMessageMove(int x, int y);
void MouseMessageRaw();

// Game thread, once per frame after the pad update: while the fallback is
// on, writes the frame's mouse delta from the window messages into the
// game's mouse state and pins the pointer back to the centre.
void MouseFallbackFrame();

// Worker thread, every quarter second: decides whether the mouse is dead and
// runs the rescue steps.
void WatchMouse();

// One line for the input log: calls, results, motion, messages, fallback.
std::string MouseWatchLine();

bool MouseFallback();
int  MouseRescues();

}  // namespace gtabot::game
