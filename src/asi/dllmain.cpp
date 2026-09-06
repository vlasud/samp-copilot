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

#include <atomic>

#include "bridge.hpp"
#include "crash_log.hpp"
#include "hooks/frame.hpp"
#include "log.hpp"
#include "mcp/http.hpp"
#include "mcp/server.hpp"
#include "mcp/tools.hpp"
#include "samp/version.hpp"
#include "state/probe.hpp"
#include "types.hpp"
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

std::atomic<bool> g_running{false};

// Game thread. Everything that touches client memory happens here, and only
// here.
void OnFrame() {
  Bridge::RunPending(kTasksPerFrame);

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
  while (g_running.load(std::memory_order_acquire)) {
    FrameHook::AdoptGameDevice();
    StatusSource::Mcp current = StatusSource::mcp();
    current.requests = transport.requests();
    StatusSource::SetMcp(current);
    Sleep(250);
  }

  LOG_INFO("bot.asi stopping");
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
