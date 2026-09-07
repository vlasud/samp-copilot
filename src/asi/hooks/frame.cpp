#include "hooks/frame.hpp"

#include <windows.h>
#include <d3d9.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>

#include "log.hpp"
#include "state/memory.hpp"
#include "hooks/windowmode.hpp"
#include "ui/overlay.hpp"

namespace gtabot::asi {
namespace {

// IDirect3DDevice9 vtable slots, fixed by the COM interface layout.
constexpr int kResetSlot    = 16;
constexpr int kPresentSlot  = 17;
constexpr int kEndSceneSlot = 42;

using PresentFn  = HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*,
                                      HWND, const RGNDATA*);
using EndSceneFn = HRESULT(APIENTRY*)(IDirect3DDevice9*);
using ResetFn    = HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreateFn   = IDirect3D9*(WINAPI*)(UINT);
PresentFn  g_original_present  = nullptr;
EndSceneFn g_original_endscene = nullptr;
ResetFn    g_original_reset    = nullptr;


// Entry points we patched, plus the bytes MinHook left there. Comparing the
// two later is what tells a stalled game apart from an unhooked one.
constexpr std::size_t kPatchBytes = 5;  // x86 relative jump
void*         g_present_target  = nullptr;
void*         g_endscene_target = nullptr;
void*         g_reset_target    = nullptr;

// The first device the game hands us, and whether its vtable has been examined.
std::atomic<void*> g_game_device{nullptr};
std::atomic<bool>  g_device_adopted{false};
unsigned char g_present_patch[kPatchBytes]  = {};
unsigned char g_endscene_patch[kPatchBytes] = {};

FrameCallback              g_callback;
std::atomic<bool>          g_installed{false};
std::atomic<bool>          g_present_seen{false};
std::atomic<unsigned long long> g_frames{0};
std::atomic<double>        g_fps{0.0};
std::atomic<unsigned long long> g_last_frame_ms{0};

// The callback must never be re-entered if it somehow causes another present.
thread_local bool t_in_callback = false;

void Tick() {
  const unsigned long long n =
      g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  g_last_frame_ms.store(GetTickCount64(), std::memory_order_relaxed);

  static unsigned long long last_count = 0;
  static ULONGLONG          last_ms    = 0;
  const ULONGLONG now = GetTickCount64();
  if (last_ms == 0) {
    last_ms = now;
  } else if (now - last_ms >= 1000) {
    g_fps.store(static_cast<double>(n - last_count) * 1000.0 /
                    static_cast<double>(now - last_ms),
                std::memory_order_relaxed);
    last_count = n;
    last_ms    = now;
  }

  if (t_in_callback || !g_callback) return;
  t_in_callback = true;
  // A throwing callback would unwind through the game's render code, so the
  // boundary is sealed here.
  try {
    g_callback();
  } catch (const std::exception& e) {
    LOG_ERROR("frame callback threw: {}", e.what());
  } catch (...) {
    LOG_ERROR("frame callback threw a non-standard exception");
  }
  t_in_callback = false;
}

// The overlay is a debugging aid living inside someone's running game. A fault
// in it must cost the panel, not the session - and it must say so in the log.
void RenderOverlayGuarded(IDirect3DDevice9* device) {
  __try {
    Overlay::Render(device);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Overlay::DisableAfterFault();
  }
}

HRESULT APIENTRY HookedPresent(IDirect3DDevice9* device, const RECT* src,
                               const RECT* dest, HWND window,
                               const RGNDATA* dirty) {
  g_present_seen.store(true, std::memory_order_relaxed);
  g_game_device.store(device, std::memory_order_relaxed);
  Tick();

  // Present keeps being called when EndScene has stopped, so the focus check
  // belongs here too - this is the last place we are guaranteed to run before
  // whoever owns the device decides to reset it.
  Overlay::ReleaseIfUnfocused();

  const HRESULT hr = g_original_present(device, src, dest, window, dirty);

  // The reliable signal that the device just went away - alt-tabbing out of
  // exclusive fullscreen surfaces here first. Waiting for the Reset hook is
  // not enough: the game may stop calling EndScene entirely once it is lost,
  // and Reset then fails because we are still holding resources.
  if (hr == D3DERR_DEVICELOST) Overlay::OnLostDevice();
  return hr;
}

HRESULT APIENTRY HookedEndScene(IDirect3DDevice9* device) {
  g_game_device.store(device, std::memory_order_relaxed);
  // RenderWare can present through a swap chain instead of the device, in
  // which case Present never fires and EndScene has to drive the tick. Once
  // Present is seen, EndScene stops ticking so no frame is counted twice.
  if (!g_present_seen.load(std::memory_order_relaxed)) Tick();

  // The overlay draws here rather than in Present: this is the one place where
  // the device is still inside a BeginScene/EndScene pair and will take our
  // geometry.
  RenderOverlayGuarded(device);
  return g_original_endscene(device);
}

HRESULT APIENTRY HookedReset(IDirect3DDevice9* device,
                             D3DPRESENT_PARAMETERS* params) {
  // Logged once so the log answers whether this hook fires at all - slot 16 is
  // owned by apphelp.dll here, not d3d9, and a shim is not guaranteed to route
  // the game's own device through the same function.
  // Logged every time, not just once: whether this hook runs at all during a
  // failing reset is the whole question, and another module here resets the
  // device without going through it.
  LOG_INFO("Reset hook entered");
  // Alt-tabbing out of exclusive fullscreen loses the device; coming back
  // resets it. Anything holding D3D resources has to let go first, or the
  // reset fails and the next frame draws with dead handles.
  Overlay::OnLostDevice();
  // The only place windowed mode is applied. Forcing it when the device is
  // first created makes the game quit: RenderWare checks the device against
  // the exclusive video mode it selected and refuses a windowed one. A reset
  // is different - the game has finished initialising, has released its own
  // resources, and is asking for the device back, so handing it a windowed
  // one is a change it is already built to absorb.
  WindowMode::ForceWindowed(params);
  const HRESULT hr = g_original_reset(device, params);
  LOG_INFO("Reset returned 0x{:08X}", static_cast<unsigned int>(hr));
  if (SUCCEEDED(hr) && params != nullptr)
    WindowMode::ApplyWindowStyle(params->hDeviceWindow,
                                 static_cast<int>(params->BackBufferWidth),
                                 static_cast<int>(params->BackBufferHeight));
  return hr;
}


// Creates a device only to read its vtable, then tears everything down, so no
// device of ours lingers to compete with the game's.
bool ResolveVTable(void** present_out, void** endscene_out, void** reset_out) {
  HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
  if (!d3d9) {
    LOG_ERROR("d3d9.dll is not loadable");
    return false;
  }
  auto create = reinterpret_cast<CreateFn>(GetProcAddress(d3d9, "Direct3DCreate9"));
  if (!create) {
    LOG_ERROR("Direct3DCreate9 is not exported");
    return false;
  }
  IDirect3D9* d3d = create(D3D_SDK_VERSION);
  if (!d3d) {
    LOG_ERROR("Direct3DCreate9 returned null");
    return false;
  }

  WNDCLASSEXW wc{};
  wc.cbSize        = sizeof(wc);
  wc.lpfnWndProc   = DefWindowProcW;
  wc.hInstance     = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"gtabot_probe";
  RegisterClassExW(&wc);
  HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0,
                                0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);

  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed         = TRUE;
  pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.hDeviceWindow    = window;

  IDirect3DDevice9* device = nullptr;
  HRESULT hr = d3d->CreateDevice(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp,
      &device);
  if (FAILED(hr)) {
    // No usable hardware device yet: the reference device has the same vtable,
    // which is all we are after.
    hr = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp,
        &device);
  }

  bool ok = false;
  if (SUCCEEDED(hr) && device) {
    void** vtable = *reinterpret_cast<void***>(device);
    *present_out  = vtable[kPresentSlot];
    *endscene_out = vtable[kEndSceneSlot];
    *reset_out    = vtable[kResetSlot];
    device->Release();
    ok = true;
  } else {
    LOG_ERROR("CreateDevice failed, hr=0x{:08X}",
              static_cast<unsigned int>(hr));
  }

  d3d->Release();
  if (window) DestroyWindow(window);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return ok;
}

}  // namespace

bool FrameHook::Install(FrameCallback on_frame) {
  if (g_installed.load(std::memory_order_acquire)) return true;

  void* present  = nullptr;
  void* endscene = nullptr;
  void* reset    = nullptr;
  if (!ResolveVTable(&present, &endscene, &reset)) return false;
  // Which module owns each slot matters: another overlay (NVIDIA, Steam,
  // sampvoice) may already have replaced some of them with its own handlers,
  // in which case we are chaining onto its hook rather than onto d3d9.
  LOG_INFO("Present  -> {}", mem::DescribeAddress(
                                 reinterpret_cast<std::uintptr_t>(present)));
  LOG_INFO("EndScene -> {}", mem::DescribeAddress(
                                 reinterpret_cast<std::uintptr_t>(endscene)));
  LOG_INFO("Reset    -> {}", mem::DescribeAddress(
                                 reinterpret_cast<std::uintptr_t>(reset)));

  if (MH_Initialize() != MH_OK) {
    LOG_ERROR("MH_Initialize failed");
    return false;
  }
  g_callback = std::move(on_frame);

  const MH_STATUS p = MH_CreateHook(present, &HookedPresent,
                                    reinterpret_cast<void**>(&g_original_present));
  const MH_STATUS e = MH_CreateHook(endscene, &HookedEndScene,
                                    reinterpret_cast<void**>(&g_original_endscene));
  const MH_STATUS r = MH_CreateHook(reset, &HookedReset,
                                    reinterpret_cast<void**>(&g_original_reset));
  if (r != MH_OK)
    LOG_ERROR("Reset is unhooked ({}) - the overlay will not survive a device "
              "loss", static_cast<int>(r));
  if (p != MH_OK && e != MH_OK) {
    LOG_ERROR("MH_CreateHook failed for both entry points ({}, {})",
              static_cast<int>(p), static_cast<int>(e));
    MH_Uninitialize();
    return false;
  }
  if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
    LOG_ERROR("MH_EnableHook failed");
    MH_Uninitialize();
    return false;
  }

  g_present_target  = p == MH_OK ? present : nullptr;
  g_endscene_target = e == MH_OK ? endscene : nullptr;
  g_reset_target    = r == MH_OK ? reset : nullptr;
  if (g_present_target)
    memcpy(g_present_patch, g_present_target, kPatchBytes);
  if (g_endscene_target)
    memcpy(g_endscene_patch, g_endscene_target, kPatchBytes);

  g_installed.store(true, std::memory_order_release);
  LOG_INFO("frame hook installed (Present={}, EndScene={})",
           p == MH_OK ? "ok" : "failed", e == MH_OK ? "ok" : "failed");
  return true;
}

void FrameHook::Uninstall() {
  if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();
  Overlay::Shutdown();
  g_callback        = nullptr;
  g_present_target  = nullptr;
  g_endscene_target = nullptr;
  g_reset_target    = nullptr;
}

std::uint64_t FrameHook::idle_ms() {
  const unsigned long long last =
      g_last_frame_ms.load(std::memory_order_relaxed);
  if (last == 0) return 0;
  const ULONGLONG now = GetTickCount64();
  return now > last ? now - last : 0;
}

FrameHook::Integrity FrameHook::CheckIntegrity() {
  Integrity out;
  if (!g_installed.load(std::memory_order_acquire)) return out;

  if (g_present_target) {
    out.present_hooked = true;
    out.present_byte   = *static_cast<const unsigned char*>(g_present_target);
    out.present_intact =
        memcmp(g_present_target, g_present_patch, kPatchBytes) == 0;
  }
  if (g_endscene_target) {
    out.endscene_hooked = true;
    out.endscene_byte   = *static_cast<const unsigned char*>(g_endscene_target);
    out.endscene_intact =
        memcmp(g_endscene_target, g_endscene_patch, kPatchBytes) == 0;
  }
  return out;
}

void FrameHook::AdoptGameDevice() {
  if (!g_installed.load(std::memory_order_acquire)) return;
  if (g_device_adopted.load(std::memory_order_acquire)) return;

  auto* device = static_cast<IDirect3DDevice9*>(
      g_game_device.load(std::memory_order_relaxed));
  if (!device) return;
  g_device_adopted.store(true, std::memory_order_release);

  void** vtable = *reinterpret_cast<void***>(device);
  void*  actual = vtable[kResetSlot];

  LOG_INFO("game device Reset -> {} (we hooked {})",
           mem::DescribeAddress(reinterpret_cast<std::uintptr_t>(actual)),
           mem::DescribeAddress(reinterpret_cast<std::uintptr_t>(g_reset_target)));
  if (actual == g_reset_target) {
    LOG_INFO("Reset hook already covers the game's device");
    return;
  }

  if (g_reset_target) MH_RemoveHook(g_reset_target);
  g_original_reset = nullptr;
  if (MH_CreateHook(actual, &HookedReset,
                    reinterpret_cast<void**>(&g_original_reset)) != MH_OK ||
      MH_EnableHook(actual) != MH_OK) {
    LOG_ERROR("could not hook the game device's Reset - a device loss will "
              "still fail");
    g_reset_target = nullptr;
    return;
  }
  g_reset_target = actual;
  LOG_INFO("Reset hook moved onto the game device's own entry point");
}

bool FrameHook::installed() { return g_installed.load(std::memory_order_acquire); }

std::uint64_t FrameHook::frames() {
  return g_frames.load(std::memory_order_relaxed);
}

double FrameHook::fps() { return g_fps.load(std::memory_order_relaxed); }

const char* FrameHook::driver() {
  if (!g_installed.load(std::memory_order_acquire)) return "none";
  return g_present_seen.load(std::memory_order_relaxed) ? "Present" : "EndScene";
}

}  // namespace gtabot::asi
