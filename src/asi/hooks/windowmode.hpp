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
// It is applied at exactly one moment: when the game resets its own device.
// Forcing it when the device is first created does not work here - GTA checks
// the device against the exclusive video mode RenderWare selected and quits
// when it does not match, which it did, twice. A reset is different: the game
// has finished initialising, has released its own resources, and is asking
// for the device back, so a windowed one is a change it is already built to
// absorb. The practical consequence is that the game starts fullscreen and
// becomes a window the first time the device resets - which is what alt-tab
// does. Nothing in the game's code is touched: only the parameters our own
// D3D hook is handed, and the window's own style.
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

  // Whether the walker may patch the game's own code. gta_sa.exe here is
  // protected - instructions relocated into stubs, junk code around them -
  // and the walker's hook is the only place this module writes into it.
  // bot.cfg: walker=on (default) or walker=off.
  static bool WalkerAllowed();

  // Whether the joystick may reach the pad. Off by default: the game
  // reconciles it with the keyboard by rules that let a gamepad on the desk
  // switch W, A, S and D off. bot.cfg `gamepad=on` lets it through.
  static bool GamepadAllowed();

  // Whether the game carries on while somebody else is using the computer.
  //
  // GTA stops when it loses focus: no frames, and a game that draws no
  // frames runs no script, sends no packets and takes no steps. That is
  // correct for a person playing, and useless for a character living his
  // own life on somebody's second monitor - every measurement taken here
  // while the window was behind a browser read nought metres walked.
  //
  // On, the module keeps the news of the loss from the game and sends keys
  // as messages to the window rather than through the system, so they reach
  // the game and nothing else. bot.cfg `background=off` restores the plain
  // behaviour.
  static bool RunsInBackground();

  // Whether the investigation instruments run: hardware watchpoints on the
  // input switches, the Windows API trace, the DirectInput mouse hooks.
  // Off by default - each is a deviation from a plain player's process.
  // bot.cfg `diagnostics=on`.
  static bool DiagnosticsAllowed();

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
