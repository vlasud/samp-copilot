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

#include "common/pipe.hpp"
#include "common/protocol.hpp"
#include "samp/version.hpp"
#include "log.hpp"

namespace gtabot::asi {
namespace {

std::atomic<bool>       g_running{false};
ipc::PipeClient*        g_link = nullptr;
HANDLE                  g_thread = nullptr;

void OnMessage(const proto::Envelope& env) {
  if (env.v != proto::kVersion) {
    LOG_WARN("dropping message with protocol v{} (we speak v{})", env.v,
             proto::kVersion);
    return;
  }
  if (env.type == proto::msg::kAction) {
    // Actions must run on the game thread. Until the frame hook exists they
    // are only logged, so the transport can be exercised end to end first.
    LOG_INFO("action id={} kind={}", env.id,
             env.payload.value("kind", std::string{"?"}));
    return;
  }
  LOG_DEBUG("<- {}", env.type);
}

void OnLinkState(bool connected) {
  LOG_INFO("link {}", connected ? "up" : "down");
}

DWORD WINAPI Worker(LPVOID) {
  InitLogging("bot.asi.log");
  LOG_INFO("bot.asi v{} starting, protocol v{}", GTABOT_VERSION, proto::kVersion);

  // SA-MP is loaded by samp.exe well after the ASI loader runs.
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

  proto::json hello = {
      {"component", "bot.asi"},
      {"version", GTABOT_VERSION},
      {"samp", {{"version", samp::ToString(client.version)},
                {"base", client.base},
                {"size_of_image", client.size_of_image},
                {"timestamp", client.timestamp}}},
      {"pid", static_cast<std::uint32_t>(GetCurrentProcessId())},
  };

  bool announced = false;
  while (g_running.load(std::memory_order_acquire)) {
    if (g_link->connected()) {
      if (!announced) {
        announced = g_link->Send(proto::Make(proto::msg::kHello, hello));
      } else {
        // Placeholder heartbeat. The real collector replaces this with a
        // double-buffered snapshot published from the frame hook.
        g_link->Send(proto::Make(
            proto::msg::kSnapshot,
            {{"stub", true}, {"tick", GetTickCount64()}}));
      }
    } else {
      announced = false;
    }
    Sleep(1000);
  }

  LOG_INFO("bot.asi stopping");
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
      gtabot::asi::g_thread =
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
