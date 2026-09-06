#pragma once
//
// Correlates an action sent to the game with the result that comes back.
//
// MCP tool calls are synchronous from the agent's point of view, but the game
// can only answer inside its own frame, so the calling thread parks here until
// the result arrives or the wait times out.
//
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "common/protocol.hpp"

namespace gtabot::mcp {

class Rpc {
 public:
  std::uint64_t NextId();

  // Blocks. Throws std::runtime_error on timeout so the tool reports a clean
  // failure instead of hanging the session forever.
  proto::json Await(std::uint64_t id, int timeout_ms);

  // Called from the IO thread when a `result` envelope arrives.
  void Complete(std::uint64_t id, proto::json payload);

  // Fails every outstanding wait, e.g. when the game disconnects.
  void FailAll(const std::string& reason);

 private:
  std::mutex              mutex_;
  std::condition_variable arrived_;
  std::map<std::uint64_t, proto::json> results_;
  std::uint64_t next_id_ = 1;
  bool          aborted_ = false;
  std::string   abort_reason_;
};

}  // namespace gtabot::mcp
