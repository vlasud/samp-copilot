#include "mcp/rpc.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>

#include "bridge.hpp"

namespace gtabot::mcp {
namespace {

std::atomic<std::uint64_t> g_in_flight{0};
std::atomic<std::uint64_t> g_timed_out{0};

// Shared with the posted task. It outlives a timed-out caller, so the game
// thread can still complete without writing into a dead stack frame.
struct Slot {
  std::mutex              mutex;
  std::condition_variable done;
  json                    result;
  std::string             error;
  bool                    finished = false;
};

}  // namespace

json Rpc::RunOnGameThread(std::function<json()> work, int timeout_ms) {
  auto slot = std::make_shared<Slot>();

  asi::Bridge::PostToGameThread([slot, work = std::move(work)]() {
    json    result;
    std::string error;
    try {
      result = work();
    } catch (const std::exception& e) {
      error = e.what();
    } catch (...) {
      error = "the game-thread task threw a non-standard exception";
    }
    {
      std::lock_guard<std::mutex> lock(slot->mutex);
      slot->result   = std::move(result);
      slot->error    = std::move(error);
      slot->finished = true;
    }
    slot->done.notify_all();
  });

  g_in_flight.fetch_add(1, std::memory_order_relaxed);
  std::unique_lock<std::mutex> lock(slot->mutex);
  const bool finished = slot->done.wait_for(
      lock, std::chrono::milliseconds(timeout_ms),
      [&slot] { return slot->finished; });
  g_in_flight.fetch_sub(1, std::memory_order_relaxed);

  if (!finished) {
    g_timed_out.fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error(
        "the game thread did not run this within " + std::to_string(timeout_ms) +
        " ms - it only runs while the game is rendering, so check whether the "
        "window is minimised (see bot_status.verdict)");
  }
  if (!slot->error.empty()) throw std::runtime_error(slot->error);
  return slot->result;
}

std::uint64_t Rpc::in_flight() {
  return g_in_flight.load(std::memory_order_relaxed);
}

std::uint64_t Rpc::timed_out() {
  return g_timed_out.load(std::memory_order_relaxed);
}

}  // namespace gtabot::mcp
