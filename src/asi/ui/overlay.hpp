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

  // Releases every D3DPOOL_DEFAULT resource we hold. Idempotent, and it has to
  // be: Reset fails with D3DERR_INVALIDCALL while any of them still exists,
  // and the game shows its own error box and gives up.
  static void OnLostDevice();
  static void OnResetDevice();

  // Drops our D3D resources as soon as the game window stops being the
  // foreground one.
  //
  // Another module in this process (vc.asi) owns the device across an alt-tab
  // and calls Reset itself, without going through any hook of ours. Waiting to
  // be told is therefore not an option: focus loss happens before the device
  // does, and it is a signal we can read ourselves.
  static void ReleaseIfUnfocused();

  static void Shutdown();

  // Called by the frame hook when drawing faulted. The panel is a debugging
  // aid; it does not get to take someone's session with it, so it switches
  // itself off for good and the game carries on.
  static void DisableAfterFault();
  static bool disabled();

  static bool visible();
  static void SetVisible(bool visible);
};

}  // namespace gtabot::asi
