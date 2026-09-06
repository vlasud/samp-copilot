#include "rpc.hpp"

#include <chrono>
#include <stdexcept>

namespace gtabot::mcp {

std::uint64_t Rpc::NextId() {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_id_++;
}

proto::json Rpc::Await(std::uint64_t id, int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  aborted_ = false;
  const bool signalled = arrived_.wait_for(
      lock, std::chrono::milliseconds(timeout_ms),
      [this, id] { return aborted_ || results_.count(id) != 0; });

  const auto it = results_.find(id);
  if (it != results_.end()) {
    proto::json payload = std::move(it->second);
    results_.erase(it);
    return payload;
  }
  if (aborted_) throw std::runtime_error(abort_reason_);
  if (!signalled)
    throw std::runtime_error("the game did not answer within " +
                             std::to_string(timeout_ms) + " ms");
  throw std::runtime_error("no result for action " + std::to_string(id));
}

void Rpc::Complete(std::uint64_t id, proto::json payload) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    results_[id] = std::move(payload);
  }
  arrived_.notify_all();
}

void Rpc::FailAll(const std::string& reason) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_      = true;
    abort_reason_ = reason;
  }
  arrived_.notify_all();
}

}  // namespace gtabot::mcp
