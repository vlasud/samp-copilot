#pragma once
//
// MCP over HTTP, served from inside the game process.
//
// This replaces the separate executable and the named pipe: an agent connects
// to http://127.0.0.1:<port>/mcp and POSTs JSON-RPC. The listener binds to the
// loopback address only - nothing here should ever be reachable off the box.
//
// One connection is handled at a time on a single worker thread. MCP clients
// are sequential, and serialising requests means a tool handler can block on
// the game thread without starving anything else.
//
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace gtabot::mcp {

class Server;

class HttpTransport {
 public:
  ~HttpTransport();

  // Binds and starts serving. `server` must outlive the transport.
  bool Start(Server* server, std::uint16_t port);
  void Stop();

  bool          running() const { return running_.load(std::memory_order_acquire); }
  std::uint16_t port() const { return port_; }
  std::uint64_t requests() const { return requests_.load(std::memory_order_relaxed); }
  // Empty until Start succeeds or fails; holds the reason on failure.
  const std::string& last_error() const { return last_error_; }

 private:
  void Run();
  void ServeConnection(std::uintptr_t client);

  Server*           server_ = nullptr;
  std::uintptr_t    listener_ = ~std::uintptr_t{0};
  std::thread       thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> requests_{0};
  std::uint16_t     port_ = 0;
  std::string       last_error_;
};

}  // namespace gtabot::mcp
