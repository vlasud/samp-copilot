#pragma once
//
// Named-pipe transport for the NDJSON protocol in protocol.hpp.
//
// gta-mcp.exe owns the pipe (PipeServer) and bot.asi dials into it
// (PipeClient), so the game can be restarted as often as you like without the
// collected state or the MCP session going away. Both endpoints reconnect on
// their own; neither blocks the caller's thread.
//
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "protocol.hpp"

namespace gtabot::ipc {

// Called on the endpoint's own IO thread. Keep the body short: for the ASI side
// this is *not* the game thread, but anything it touches must be synchronised
// with it.
using MessageHandler = std::function<void(const proto::Envelope&)>;
using StateHandler   = std::function<void(bool connected)>;

class Endpoint {
 public:
  Endpoint() = default;
  virtual ~Endpoint();

  Endpoint(const Endpoint&)            = delete;
  Endpoint& operator=(const Endpoint&) = delete;

  // Spawns the IO thread. Returns immediately; `on_state` reports when a peer
  // is actually attached.
  bool Start(MessageHandler on_message, StateHandler on_state = {});
  void Stop();

  // Thread-safe. Fails (returns false) when no peer is attached; the caller
  // decides whether that is worth queueing or dropping.
  bool Send(const proto::Envelope& env);

  bool connected() const { return connected_.load(std::memory_order_acquire); }

 protected:
  // Blocks until a peer is attached or `stop_` fires. Returns an invalid
  // handle on shutdown. Implemented per endpoint role.
  virtual void* AcquirePeer() = 0;

  void*             stop_event_ = nullptr;
  std::atomic<bool> connected_{false};

 private:
  void Run();
  void PumpPeer(void* handle);
  void DispatchLine(const std::string& line);

  MessageHandler    on_message_;
  StateHandler      on_state_;
  std::thread       thread_;
  std::mutex        write_mutex_;
  void*             write_handle_ = nullptr;  // guarded by write_mutex_
  std::string       inbox_;                   // partial line carry-over
};

class PipeServer final : public Endpoint {
 protected:
  void* AcquirePeer() override;
};

class PipeClient final : public Endpoint {
 protected:
  void* AcquirePeer() override;
};

}  // namespace gtabot::ipc
