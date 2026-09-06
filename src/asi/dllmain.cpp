//
// bot.asi - loaded into gta_sa.exe by the ASI loader already present in the
// game folder (vorbisFile.dll).
//
// DllMain does nothing but spawn a worker: the loader holds the loader lock
// while calling us, and anything that touches another module from in there
// deadlocks sooner or later.
//
#include <windows.h>

#include <atomic>
#include <string>

#include "bridge.hpp"
#include "common/pipe.hpp"
#include "common/protocol.hpp"
#include "hooks/frame.hpp"
#include "log.hpp"
#include "samp/version.hpp"
#include "state/probe.hpp"

namespace gtabot::asi {
namespace {

// A snapshot four times a second is plenty for an agent and invisible in frame
// time; at 60 fps that is one build every fifteenth frame.
constexpr unsigned long long kFramesPerSnapshot = 15;
// Bounded so a burst of queued work cannot turn into a frame spike.
constexpr std::size_t kTasksPerFrame = 4;
constexpr DWORD       kWorkerPeriodMs = 50;

std::atomic<bool> g_running{false};
ipc::PipeClient*  g_link = nullptr;

// Game thread. Everything that touches client memory happens here and nowhere
// else.
void OnFrame() {
  Bridge::RunPending(kTasksPerFrame);

  static unsigned long long last_snapshot_frame = 0;
  const unsigned long long frame = FrameHook::frames();
  if (frame - last_snapshot_frame >= kFramesPerSnapshot) {
    last_snapshot_frame = frame;
    Bridge::Publish(proto::Make(proto::msg::kSnapshot, BuildSnapshot()));
  }
}

void HandleAction(const proto::Envelope& env) {
  const std::string kind = env.payload.value("kind", std::string{});
  const std::uint64_t id = env.id;

  if (kind == proto::action::kProbeMemory) {
    const proto::json args = env.payload;
    Bridge::PostToGameThread([id, args]() {
      try {
        Bridge::Publish(proto::Make(proto::msg::kResult,
                                    {{"ok", true}, {"data", ProbeMemory(args)}},
                                    id));
      } catch (const std::exception& e) {
        Bridge::Publish(proto::Make(proto::msg::kResult,
                                    {{"ok", false}, {"error", e.what()}}, id));
      }
    });
    return;
  }

  // Everything else is still a stub. Saying so beats reporting a success the
  // agent would then build on.
  LOG_WARN("unimplemented action: {}", kind);
  Bridge::Publish(proto::Make(
      proto::msg::kResult,
      {{"ok", false}, {"error", "action not implemented yet: " + kind}}, id));
}

void OnMessage(const proto::Envelope& env) {
  if (env.v != proto::kVersion) {
    LOG_WARN("dropping message with protocol v{} (we speak v{})", env.v,
             proto::kVersion);
    return;
  }
  if (env.type == proto::msg::kAction) {
    HandleAction(env);
    return;
  }
  LOG_DEBUG("<- {}", env.type);
}

void OnLinkState(bool connected) { LOG_INFO("link {}", connected ? "up" : "down"); }

DWORD WINAPI Worker(LPVOID) {
  InitLogging("bot.asi.log");
  LOG_INFO("bot.asi v{} starting, protocol v{}", GTABOT_VERSION, proto::kVersion);

  // Install before waiting for SA-MP: the hook needs no client, and having it
  // running early means the frame counter itself becomes a liveness signal.
  if (!FrameHook::Install(&OnFrame))
    LOG_ERROR("frame hook could not be installed - no game-thread access");

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
  }

  g_link = new ipc::PipeClient();
  if (!g_link->Start(&OnMessage, &OnLinkState)) {
    LOG_ERROR("failed to start the IPC client");
    return 1;
  }

  const proto::json hello = {
      {"component", "bot.asi"},
      {"version", GTABOT_VERSION},
      {"samp",
       {{"version", samp::ToString(client.version)},
        {"base", client.base},
        {"size_of_image", client.size_of_image},
        {"timestamp", client.timestamp}}},
      {"frame_hook", FrameHook::installed()},
      {"pid", static_cast<std::uint32_t>(GetCurrentProcessId())},
  };

  bool announced = false;
  while (g_running.load(std::memory_order_acquire)) {
    if (g_link->connected()) {
      if (!announced) announced = g_link->Send(proto::Make(proto::msg::kHello, hello));
      if (announced) {
        for (proto::Envelope& env : Bridge::DrainOutbox()) {
          if (!g_link->Send(env)) break;
        }
      }
    } else {
      announced = false;
    }
    Sleep(kWorkerPeriodMs);
  }

  LOG_INFO("bot.asi stopping");
  FrameHook::Uninstall();
  g_link->Stop();
  delete g_link;
  g_link = nullptr;
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
