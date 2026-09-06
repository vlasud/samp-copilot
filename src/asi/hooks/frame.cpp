#include "hooks/frame.hpp"

#include <windows.h>
#include <d3d9.h>

#include <MinHook.h>

#include <atomic>

#include "log.hpp"

namespace gtabot::asi {
namespace {

// IDirect3DDevice9 vtable slots, fixed by the COM interface layout.
constexpr int kPresentSlot  = 17;
constexpr int kEndSceneSlot = 42;

using PresentFn  = HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*,
                                      HWND, const RGNDATA*);
using EndSceneFn = HRESULT(APIENTRY*)(IDirect3DDevice9*);
using CreateFn   = IDirect3D9*(WINAPI*)(UINT);

PresentFn  g_original_present  = nullptr;
EndSceneFn g_original_endscene = nullptr;

FrameCallback              g_callback;
std::atomic<bool>          g_installed{false};
std::atomic<bool>          g_present_seen{false};
std::atomic<unsigned long long> g_frames{0};
std::atomic<double>        g_fps{0.0};

// The callback must never be re-entered if it somehow causes another present.
thread_local bool t_in_callback = false;

void Tick() {
  const unsigned long long n =
      g_frames.fetch_add(1, std::memory_order_relaxed) + 1;

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

HRESULT APIENTRY HookedPresent(IDirect3DDevice9* device, const RECT* src,
                               const RECT* dest, HWND window,
                               const RGNDATA* dirty) {
  g_present_seen.store(true, std::memory_order_relaxed);
  Tick();
  return g_original_present(device, src, dest, window, dirty);
}

HRESULT APIENTRY HookedEndScene(IDirect3DDevice9* device) {
  // RenderWare can present through a swap chain instead of the device, in
  // which case Present never fires and EndScene has to drive the tick. Once
  // Present is seen, EndScene stops ticking so no frame is counted twice.
  if (!g_present_seen.load(std::memory_order_relaxed)) Tick();
  return g_original_endscene(device);
}

// Creates a device only to read its vtable, then tears everything down, so no
// device of ours lingers to compete with the game's.
bool ResolveVTable(void** present_out, void** endscene_out) {
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
  if (!ResolveVTable(&present, &endscene)) return false;
  LOG_INFO("d3d9 vtable resolved: Present=0x{:08X} EndScene=0x{:08X}",
           reinterpret_cast<unsigned int>(present),
           reinterpret_cast<unsigned int>(endscene));

  if (MH_Initialize() != MH_OK) {
    LOG_ERROR("MH_Initialize failed");
    return false;
  }
  g_callback = std::move(on_frame);

  const MH_STATUS p = MH_CreateHook(present, &HookedPresent,
                                    reinterpret_cast<void**>(&g_original_present));
  const MH_STATUS e = MH_CreateHook(endscene, &HookedEndScene,
                                    reinterpret_cast<void**>(&g_original_endscene));
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

  g_installed.store(true, std::memory_order_release);
  LOG_INFO("frame hook installed (Present={}, EndScene={})",
           p == MH_OK ? "ok" : "failed", e == MH_OK ? "ok" : "failed");
  return true;
}

void FrameHook::Uninstall() {
  if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();
  g_callback = nullptr;
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
