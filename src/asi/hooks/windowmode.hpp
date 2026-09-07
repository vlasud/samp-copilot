#pragma once
//
// Runs the game in a window instead of exclusive fullscreen.
//
// Two reasons this earns its place. Exclusive fullscreen is what turns a
// stuck game thread into a machine you have to reboot: the GPU is held, the
// desktop cannot compose, and alt-tab does not come. A window makes every one
// of those failures recoverable. And a small window is simply what a person
// developing this wants in front of them.
//
// It is done the way windowed-mode mods have always done it: force
// Windowed=TRUE in the presentation parameters at the one moment the device
// is created (and again on every reset, so it stays), then give the window a
// border and a sensible size. Nothing in the game's code is touched - only
// the parameters our own D3D hook is handed, and the window's own style.
//
// A setting, because it changes how the game presents and this is a server
// with an anticheat: bot.cfg beside the module holds `window=on`, `window=off`
// to leave the game fullscreen, or `window=WIDTHxHEIGHT` to also override the
// back buffer - which the game may refuse to start with, so it is opt-in.
// On by default, because it was asked for and because it is the safer place
// to be.
//
#include <windows.h>
#include <d3d9.h>

namespace gtabot::asi {

class WindowMode {
 public:
  // Reads bot.cfg once. Safe to call repeatedly.
  static void EnsureConfigured();
  static bool Enabled();

  // Rewrites present parameters to windowed at the configured size. Called
  // from the CreateDevice and Reset hooks with whatever the game passed.
  static void ForceWindowed(D3DPRESENT_PARAMETERS* params);

  // Gives the window a border and centres it at the size the device actually
  // got. Called after the device is (re)created. A width or height of zero
  // means the device took its size from the window, so only the frame is
  // applied.
  static void ApplyWindowStyle(HWND window, int width, int height);
};

}  // namespace gtabot::asi
