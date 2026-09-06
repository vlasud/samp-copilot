#pragma once
//
// The in-game debug panel. Everything the module knows, drawn over the game so
// there is nothing to alt-tab to.
//
// Draws inside the EndScene hook, which is the only point where Direct3D is
// between BeginScene and EndScene and will accept our geometry.
//
// It takes no input. The game owns the mouse and keyboard through DirectInput,
// and wrestling that away is a separate problem from showing numbers - so the
// panel is read-only and toggled with a single polled key.
//
struct IDirect3DDevice9;

namespace gtabot::asi {

class Overlay {
 public:
  // Render-thread only. Initialises itself on the first call, and re-does that
  // if the game hands us a different device than last time.
  static void Render(IDirect3DDevice9* device);

  // Around the game's own Reset. Skipping these turns the first alt-tab back
  // into the game into a crash.
  static void OnLostDevice();
  static void OnResetDevice();

  static void Shutdown();

  static bool visible();
  static void SetVisible(bool visible);
};

}  // namespace gtabot::asi
