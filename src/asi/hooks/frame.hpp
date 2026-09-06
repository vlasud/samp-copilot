#pragma once
//
// A callback that fires once per rendered frame, on the game's own thread.
//
// This is the only place from which SA-MP and GTA structures may be touched:
// they are created, mutated and freed by that thread, and reading them from
// anywhere else eventually returns torn data or dereferences freed memory.
//
// The hook is placed on Direct3D9 itself rather than on a hardcoded address
// inside gta_sa.exe. A throwaway device is created just long enough to read the
// addresses of Present and EndScene out of its vtable, then released; those
// addresses belong to d3d9.dll's shared implementation, so hooking them catches
// the game's device too. Nothing here depends on the game or client version.
//
#include <cstdint>
#include <functional>

namespace gtabot::asi {

// Runs on the render thread. Must be short: whatever it spends is frame time.
using FrameCallback = std::function<void()>;

class FrameHook {
 public:
  // Safe to call before the game has created its own device - in fact that is
  // the expected order, since the ASI loads while gta_sa.exe is still
  // resolving its imports.
  static bool Install(FrameCallback on_frame);
  static void Uninstall();

  static bool          installed();
  static std::uint64_t frames();
  // Frames per second over the last second, measured by the hook itself.
  static double        fps();
  // Which entry point is actually driving the tick, for diagnostics.
  static const char*   driver();
};

}  // namespace gtabot::asi
