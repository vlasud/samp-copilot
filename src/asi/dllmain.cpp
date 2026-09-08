//
// bot.asi - loaded into gta_sa.exe by the ASI loader already present in the
// game folder (vorbisFile.dll).
//
// Everything lives in this one module: the frame hook, the state collector,
// the debug overlay, and the MCP server an agent talks to over loopback HTTP.
// There is no second process and no IPC.
//
// DllMain does nothing but spawn a worker: the loader holds the loader lock
// while calling us, and anything that touches another module from in there
// deadlocks sooner or later.
//
#include <windows.h>
#include <psapi.h>

#include <atomic>

#include <set>
#include <string>

#include "actions/driver.hpp"
#include "actions/experiments.hpp"
#include "actions/travel.hpp"
#include "actions/walker.hpp"
#include "bridge.hpp"
#include "crash_log.hpp"
#include "game/input_probe.hpp"
#include "game/mouse_watch.hpp"
#include "game/api_trace.hpp"
#include "game/watchpoint.hpp"
#include "samp/input_state.hpp"
#include "samp/keys.hpp"
#include "samp/login.hpp"
#include "game/pad_watch.hpp"
#include "game/world_query.hpp"
#include "hooks/frame.hpp"
#include "hooks/windowmode.hpp"
#include "log.hpp"
#include "mcp/http.hpp"
#include "mcp/server.hpp"
#include "mcp/tools.hpp"
#include "samp/discovery.hpp"
#include "samp/version.hpp"
#include "state/memory.hpp"
#include "state/events.hpp"
#include "state/probe.hpp"
#include "types.hpp"
#include "ui/overlay.hpp"
#include "ui/status_source.hpp"

namespace gtabot::asi {
namespace {

// Loopback only. An agent connects to http://127.0.0.1:8765/mcp.
constexpr std::uint16_t kMcpPort = 8765;

// A world rebuild four times a second is plenty for an agent and invisible in
// frame time; at 60 fps that is one build every fifteenth frame.
constexpr unsigned long long kFramesPerWorldBuild = 15;
// Bounded so a burst of queued work cannot turn into a frame spike.
constexpr std::size_t kTasksPerFrame = 4;
// The player pool only exists once the client is in a server, so the structure
// report has to keep asking. Each attempt sweeps memory and costs a hitch, so
// they are spaced out and give up rather than nagging forever.
constexpr int kReportIntervalTicks = 80;   // worker ticks of 250 ms
constexpr int kReportMaxAttempts   = 24;

std::atomic<bool> g_running{false};

// The worker's heartbeat, and the handle the game thread uses to look at it.
//
// Three runs in a row the log simply stopped a few seconds after the game
// calls were armed - the worker went quiet and nothing said why. Dead,
// blocked, or suspended by somebody else look the same from the outside, so
// the game thread, which kept rendering through all three, watches the beat
// and, when it stops, freezes the worker for an instant to read where it is.
std::atomic<unsigned long long> g_worker_beat_ms{0};
HANDLE g_worker_handle = nullptr;
constexpr unsigned long long kWorkerSilenceMs = 3000;

// Whether a module name is one of Windows' own or a graphics driver - the
// noise in a module list, as opposed to the mods and clients in it.
bool IsSystemModule(std::string name) {
  for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (name.rfind("api-ms", 0) == 0 || name.rfind("ext-ms", 0) == 0) return true;
  if (name.rfind("nv", 0) == 0 || name.rfind("amd", 0) == 0 || name.rfind("ig", 0) == 0)
    return true;
  static const char* const kSystem[] = {
      "ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll", "gdi32.dll",
      "win32u.dll", "gdi32full.dll", "msvcp_win.dll", "ucrtbase.dll",
      "advapi32.dll", "msvcrt.dll", "sechost.dll", "rpcrt4.dll", "combase.dll",
      "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll", "imm32.dll",
      "setupapi.dll", "cfgmgr32.dll", "bcrypt.dll", "bcryptprimitives.dll",
      "ws2_32.dll", "winmm.dll", "winmmbase.dll", "version.dll", "psapi.dll",
      "uxtheme.dll", "dwmapi.dll", "msctf.dll", "kernel.appcore.dll",
      "windows.storage.dll", "wldp.dll", "shcore.dll", "profapi.dll",
      "powrprof.dll", "umpdc.dll", "cryptbase.dll", "sspicli.dll", "crypt32.dll",
      "ntmarta.dll", "dinput8.dll", "hid.dll", "dsound.dll", "d3d9.dll",
      "dxgi.dll", "d3d11.dll", "dxcore.dll", "apphelp.dll", "comctl32.dll",
      "comdlg32.dll", "wintrust.dll", "msasn1.dll", "iphlpapi.dll",
      "mswsock.dll", "dnsapi.dll", "nsi.dll", "wtsapi32.dll", "winsta.dll",
      "ddraw.dll", "dciman32.dll", "devobj.dll", "wininet.dll", "urlmon.dll",
      "iertutil.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll",
      "mmdevapi.dll", "audioses.dll", "avrt.dll", "resourcepolicyclient.dll",
      "textinputframework.dll", "coremessaging.dll", "coreuicomponents.dll",
      "wintypes.dll", "propsys.dll", "clbcatq.dll", "textshaping.dll",
      "directxdatabasehelper.dll", "gameux.dll", "sxs.dll", "winhttp.dll",
      "dbghelp.dll", "userenv.dll", "netapi32.dll", "srvcli.dll", "netutils.dll",
      "wsock32.dll", "cryptsp.dll", "rsaenh.dll", "dpapi.dll", "msimg32.dll",
      "oleacc.dll", "mpr.dll", "dhcpcsvc.dll", "dhcpcsvc6.dll", "fwpuclnt.dll",
      "rasadhlp.dll", "winnsi.dll", "twinapi.appcore.dll", "usermgrcli.dll",
      "d3dcompiler_47.dll", "dxva2.dll", "windowscodecs.dll", "mfplat.dll",
  };
  for (const char* system : kSystem)
    if (name == system) return true;
  return false;
}

// Every module in the process, once, so a report of another module's hook
// or thread can be read against what was actually loaded.
void LogModules() {
  HMODULE modules[512];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
    return;
  DWORD count = needed / sizeof(HMODULE);
  if (count > 512) count = 512;
  std::string names;
  int hidden = 0;
  for (DWORD i = 0; i < count; ++i) {
    char path[MAX_PATH] = {};
    if (!GetModuleBaseNameA(GetCurrentProcess(), modules[i], path, MAX_PATH))
      continue;
    if (IsSystemModule(path)) {
      ++hidden;
      continue;
    }
    if (!names.empty()) names += ", ";
    names += path;
  }
  LOG_INFO("modules: {} loaded, {} of them Windows' own; the rest: {}", count,
           hidden, names);
}

// Worker thread, every tick. A module that arrives in the middle of a
// session - an overlay, an input host, a shim - is worth a line with the
// time on it, because whatever it changed changed then.
void WatchModules() {
  static std::set<HMODULE> seen;
  static bool primed = false;
  static int tick = 0;
  if (++tick % 8 != 0) return;   // every two seconds
  HMODULE modules[512];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
    return;
  DWORD count = needed / sizeof(HMODULE);
  if (count > 512) count = 512;
  std::string fresh;
  for (DWORD i = 0; i < count; ++i) {
    if (!seen.insert(modules[i]).second) continue;
    if (!primed) continue;
    char name[MAX_PATH] = {};
    if (!GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, MAX_PATH))
      continue;
    if (!fresh.empty()) fresh += ", ";
    fresh += name;
  }
  primed = true;
  if (!fresh.empty()) LOG_INFO("modules: newly loaded: {}", fresh);
}

// Game thread. Looks at the worker's heartbeat; when it has stopped, says
// whether the thread is gone, held by somebody, or stuck - and where.
void WatchWorker() {
  static bool reported = false;
  static int  reports  = 0;
  static unsigned long long last_report_ms = 0;
  const unsigned long long beat = g_worker_beat_ms.load(std::memory_order_acquire);
  if (beat == 0 || g_worker_handle == nullptr) return;
  const unsigned long long now = GetTickCount64();
  const unsigned long long silent = now > beat ? now - beat : 0;
  if (silent < kWorkerSilenceMs) {
    if (reported) {
      reported = false;
      reports  = 0;
      LOG_WARN("worker thread is ticking again");
    }
    return;
  }
  if (reported && (reports >= 3 || now - last_report_ms < 10000)) return;
  reported = true;
  ++reports;
  last_report_ms = now;

  DWORD exit_code = 0;
  GetExitCodeThread(g_worker_handle, &exit_code);
  if (exit_code != STILL_ACTIVE) {
    LOG_ERROR("worker thread has EXITED (code 0x{:X}) - silent for {} ms",
              static_cast<unsigned>(exit_code), silent);
    return;
  }

  // Suspend, read, resume - and only then log. Nothing between suspend and
  // resume may take a lock the worker might be holding: no logging, no heap.
  std::uintptr_t eip = 0, esp = 0, ebp = 0;
  std::uintptr_t frames[12] = {};
  int frame_count = 0;
  bool got_context = false;
  const DWORD previous_suspends = SuspendThread(g_worker_handle);
  if (previous_suspends != static_cast<DWORD>(-1)) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(g_worker_handle, &context)) {
      got_context = true;
      eip = context.Eip;
      esp = context.Esp;
      ebp = context.Ebp;
      std::uintptr_t bp = ebp;
      for (int i = 0; i < 12 && bp != 0; ++i) {
        std::uintptr_t ret = 0, next = 0;
        if (!mem::Read<std::uintptr_t>(bp + 4, &ret)) break;
        if (!mem::Read<std::uintptr_t>(bp, &next)) break;
        frames[frame_count++] = ret;
        if (next <= bp) break;
        bp = next;
      }
    }
    ResumeThread(g_worker_handle);
  }

  FILETIME created{}, exited{}, kernel{}, user{};
  GetThreadTimes(g_worker_handle, &created, &exited, &kernel, &user);
  const auto ms = [](const FILETIME& t) {
    return (static_cast<unsigned long long>(t.dwHighDateTime) << 32 |
            t.dwLowDateTime) / 10000ULL;
  };
  LOG_ERROR("worker thread silent for {} ms (report {}): suspend count before "
            "ours = {}, cpu kernel {} ms user {} ms, {} threads in the process",
            silent, reports,
            previous_suspends == static_cast<DWORD>(-1)
                ? std::string("unknown")
                : std::to_string(previous_suspends),
            ms(kernel), ms(user), game::ThreadCount());
  if (got_context) {
    LOG_ERROR("   eip={} esp=0x{:08X} ebp=0x{:08X}", mem::DescribeAddress(eip),
              static_cast<unsigned>(esp), static_cast<unsigned>(ebp));
    for (int i = 0; i < frame_count; ++i)
      LOG_ERROR("   return {}: {}", i, mem::DescribeAddress(frames[i]));
  } else {
    LOG_ERROR("   its context could not be read");
  }
}

// Game thread. Everything that touches client memory happens here, and only
// here.
// The frame SA-MP takes the keyboard in, and what this module did in the
// frames before it. The protection NOPs the call at 0x541DF5 from its
// post-Present code; the switch is read every frame, and when it flips the
// last eight frames' record goes to the log: calls into the game, whether a
// walk was running, key messages, the gate. Reads only.
struct FrameRecord {
  unsigned long long ms = 0;
  std::uint64_t frame = 0;
  int  ground = 0, los = 0, screen = 0;
  bool walking = false;
  bool armed = false;
  unsigned long long keys = 0;
  unsigned long long sent = 0;
  int  gate = 0;
  int  mode = 0;
};
constexpr int kFrameRecords = 8;
FrameRecord g_frame_records[kFrameRecords];
int  g_frame_record_at = 0;
bool g_keyboard_was_off = false;
unsigned long long g_armed_since_ms = 0;

void RecordFrame() {
  static int last_ground = 0, last_los = 0, last_screen = 0;
  static unsigned long long last_keys = 0, last_sent = 0;
  const samp::InputSwitch s = samp::ReadInputSwitch();
  if (!s.valid) return;
  const bool armed = game::Enabled();
  if (armed && g_armed_since_ms == 0) g_armed_since_ms = GetTickCount64();
  if (!armed) g_armed_since_ms = 0;

  FrameRecord& r = g_frame_records[g_frame_record_at];
  g_frame_record_at = (g_frame_record_at + 1) % kFrameRecords;
  r.ms     = GetTickCount64();
  r.frame  = FrameHook::frames();
  const int g = game::GroundCalls(), l = game::LineOfSightCalls(),
            c = game::ScreenCalls();
  r.ground = g - last_ground;
  r.los    = l - last_los;
  r.screen = c - last_screen;
  last_ground = g;
  last_los    = l;
  last_screen = c;
  const unsigned long long keys = Overlay::KeyMessages();
  r.keys = keys - last_keys;
  last_keys = keys;
  const unsigned long long sent = act::KeyEventsSent();
  r.sent = sent - last_sent;
  last_sent = sent;
  r.walking = act::Get().walking;
  r.armed   = armed;
  r.gate    = s.gate;
  r.mode    = s.mode;

  if (s.keyboard_off && !g_keyboard_was_off && s.mode == 0) {
    LOG_WARN("keyboard taken by SA-MP in frame {}: armed {} ms ago; the last {} "
             "frames, oldest first (frame: calls ground/los/screen, walking, "
             "key messages, keys sent, gate, cursor mode):", r.frame,
             g_armed_since_ms ? r.ms - g_armed_since_ms : 0, kFrameRecords);
    for (int i = 0; i < kFrameRecords; ++i) {
      const FrameRecord& f = g_frame_records[(g_frame_record_at + i) % kFrameRecords];
      if (f.ms == 0) continue;
      LOG_WARN("  frame {} ({} ms before): {}/{}/{}, walking {}, keys {}, sent {}, "
               "gate {}, mode {}", f.frame, r.ms - f.ms, f.ground, f.los, f.screen,
               f.walking, f.keys, f.sent, f.gate, f.mode);
    }
  }
  g_keyboard_was_off = s.keyboard_off;
}

void OnFrame() {
  Bridge::RunPending(kTasksPerFrame);

  static unsigned long long frames_since_watch = 0;
  if (++frames_since_watch >= 60) {
    frames_since_watch = 0;
    WatchWorker();
  }

  // The journey decides what to do next only when the walker has stopped, so
  // this costs a comparison on almost every frame.
  samp::KeysTick();
  state::WatchEvents();
  samp::WatchLogin();
  act::TravelTick();
  act::DriveTick();
  act::PadFrame();
  act::ExperimentTick();
  RecordFrame();

  static unsigned long long last_world_frame = 0;
  const unsigned long long frame = FrameHook::frames();
  if (frame - last_world_frame >= kFramesPerWorldBuild) {
    last_world_frame = frame;
    Bridge::SetWorld(BuildWorldSnapshot());
  }
}

DWORD WINAPI Worker(LPVOID) {
  InitLogging("bot.asi.log");
  LOG_INFO("bot.asi v{} starting", GTABOT_VERSION);
  InstallCrashLogger();

  // Install before waiting for SA-MP: the hook needs no client, and having it
  // running early means the frame counter itself becomes a liveness signal.
  if (!FrameHook::Install(&OnFrame))
    LOG_ERROR("frame hook could not be installed - no game-thread access");

  static mcp::Server        server("gtabot", GTABOT_VERSION);
  static mcp::HttpTransport transport;
  mcp::RegisterTools(&server);

  StatusSource::Mcp status;
  if (transport.Start(&server, kMcpPort)) {
    status.listening = true;
    status.endpoint  = "127.0.0.1:" + std::to_string(kMcpPort) + "/mcp";
  } else {
    status.endpoint = transport.last_error();
    LOG_ERROR("mcp server did not start: {}", transport.last_error());
  }
  StatusSource::SetMcp(status);

  // SA-MP is injected by samp.exe well after the ASI loader runs.
  const samp::Client client = samp::WaitForClient(/*timeout_ms=*/60'000);
  if (!client.base) {
    LOG_ERROR("samp.dll never appeared - running without a SA-MP client");
  } else {
    LOG_INFO("samp.dll base=0x{:08X} image=0x{:X} ts=0x{:08X} version={}",
             client.base, client.size_of_image, client.timestamp,
             samp::ToString(client.version));
    LogModules();
    if (client.version == samp::Version::kUnknown) {
      LOG_ERROR(
          "unrecognised SA-MP build - refusing to read client structures. "
          "Add its fingerprint to src/asi/samp/version.cpp first.");
    }

    // Probe once, unprompted, and write the result to the log. Asking for it
    // from a terminal means alt-tabbing, which parks the game thread that has
    // to run it - so the question would never get answered that way.
    Bridge::PostToGameThread([]() {
      try {
        LogProbeSummary(ProbeMemory(json::object()));
      } catch (const std::exception& e) {
        LOG_ERROR("automatic probe failed: {}", e.what());
      }
    });
  }

  // The HTTP transport and the frame hook each run on their own; this loop
  // keeps the overlay's request counter fresh and gives the frame hook a
  // chance to re-point Reset once the game's device exists.
  int report_countdown = kReportIntervalTicks;
  int report_attempts  = 0;

  while (g_running.load(std::memory_order_acquire)) {
    g_worker_beat_ms.store(GetTickCount64(), std::memory_order_release);
    FrameHook::AdoptGameDevice();

    if (!samp::report_written() && report_attempts < kReportMaxAttempts &&
        --report_countdown <= 0) {
      report_countdown = kReportIntervalTicks;
      ++report_attempts;
      Bridge::PostToGameThread([]() {
        const samp::ReportOutcome outcome = samp::WriteStructureReport();
        if (!outcome.written && !outcome.error.empty())
          LOG_INFO("structure report not ready: {}", outcome.error);
      });
    }
    // The input picture, on this thread because it has to keep working when
    // the game thread has stopped drawing. Logged only when it changes, so
    // the moment input goes away is a line with a time on it rather than
    // something to be reconstructed afterwards.
    {
      static std::string last_input_state;
      std::string now = Overlay::InputState();
      if (now != last_input_state) {
        last_input_state = now;
        LOG_INFO("input: {}", now);
      }
    }

    // The walker needs the executable recognised, which happens once the
    // game has started; trying every tick until it takes costs nothing.
    act::Install();
    // The watch on what the keyboard is reconciled with is a patch to the
    // game's code, the only one left; it comes with the diagnostics.
    if (WindowMode::DiagnosticsAllowed()) {
      game::PadWatchInstall();
      game::MouseWatchInstall();
    }

    // If the player has stopped answering his keys while the calls are armed,
    // stand them down rather than leave him stuck.
    Overlay::WatchForLostInput();
    if (WindowMode::DiagnosticsAllowed()) game::WatchMouse();
    samp::WatchSampInput();
    // The read traps on the key table and the pad: who else looks.
    game::WatchpointsInstall();
    if (WindowMode::DiagnosticsAllowed()) {
      game::ApiTraceInstall();
      game::ApiTraceDumpIfAsked();
    }
    WatchModules();

    // The way out. Everything else that could turn the panel off needs the
    // panel to be clickable, and the whole problem is that sometimes it is
    // not.
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        (GetAsyncKeyState(VK_F12) & 0x8000))
      Overlay::Disarm();
    // And the way in without the panel: Ctrl+F11 arms and disarms the game
    // calls, so a session can be run without the panel ever being
    // interactive - which is how to find out whether the interactive
    // panel has anything to do with the input going away.
    {
      static bool arm_down = false;
      const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                        (GetAsyncKeyState(VK_F11) & 0x8000);
      if (down && !arm_down) {
        game::SetEnabled(!game::Enabled());
        LOG_INFO("Ctrl+F11: movement {}", game::Enabled() ? "armed" : "off");
      }
      arm_down = down;
    }

    StatusSource::Mcp current = StatusSource::mcp();
    current.requests = transport.requests();
    StatusSource::SetMcp(current);
    Sleep(250);
  }

  LOG_INFO("bot.asi stopping");
  act::Uninstall();
  transport.Stop();
  FrameHook::Uninstall();
  ShutdownLogging();
  return 0;
}

}  // namespace
}  // namespace gtabot::asi

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(module);
      gtabot::asi::g_running.store(true, std::memory_order_release);
      // The handle is kept: the game thread uses it to look at the worker
      // when the worker stops answering.
      gtabot::asi::g_worker_handle =
          CreateThread(nullptr, 0, &gtabot::asi::Worker, nullptr, 0, nullptr);
      break;
    case DLL_PROCESS_DETACH:
      // On process teardown Windows has already killed the other threads;
      // joining here would hang. Only signal, and let the OS reclaim.
      gtabot::asi::g_running.store(false, std::memory_order_release);
      break;
    default:
      break;
  }
  return TRUE;
}
