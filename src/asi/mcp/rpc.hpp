#pragma once
//
// Turns "post work to the game thread" into a synchronous call.
//
// An MCP tool call is synchronous from the agent's point of view, but anything
// touching client memory can only run inside the frame hook. The HTTP worker
// parks here until that frame comes around, or until the wait times out.
//
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "types.hpp"

namespace gtabot::mcp {

class Rpc {
 public:
  // Posts `work` to the game thread and blocks until it returns a value.
  // Throws std::runtime_error if no frame ran within the timeout - which is
  // exactly what happens when the game is minimised and not rendering.
  static json RunOnGameThread(std::function<json()> work, int timeout_ms);

  static std::uint64_t in_flight();
  static std::uint64_t timed_out();
};

}  // namespace gtabot::mcp
