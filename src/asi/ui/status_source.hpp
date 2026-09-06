#pragma once
//
// A small noticeboard the overlay reads from.
//
// The panel draws on the render thread and must not reach into the HTTP
// transport's own state to do it; the owner publishes here instead.
//
#include <cstdint>
#include <string>

namespace gtabot::asi {

class StatusSource {
 public:
  struct Mcp {
    bool          listening = false;
    std::string   endpoint;   // e.g. "127.0.0.1:8765/mcp", or the failure reason
    std::uint64_t requests = 0;
  };

  static void SetMcp(Mcp status);
  static Mcp  mcp();
};

}  // namespace gtabot::asi
