#pragma once
//
// Everything the ASI has told us, kept so the agent can ask questions between
// game sessions. Written from the pipe's IO thread, read from the MCP thread.
//
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "common/protocol.hpp"

namespace gtabot::mcp {

class State {
 public:
  // How much recent history to keep in memory. Durable storage (SQLite) comes
  // later; this is what the tools read today.
  static constexpr std::size_t kMaxEvents = 2000;

  void OnLinkState(bool connected);
  void OnMessage(const proto::Envelope& env);

  proto::json StatusJson() const;
  proto::json RecentEvents(std::size_t limit) const;
  proto::json LatestSnapshot() const;

 private:
  mutable std::mutex mutex_;

  bool           link_up_ = false;
  std::int64_t   link_changed_ms_ = 0;
  proto::json    hello_ = nullptr;          // last handshake from the ASI
  proto::json    snapshot_ = nullptr;       // last world snapshot
  std::int64_t   snapshot_ms_ = 0;
  std::uint64_t  snapshots_seen_ = 0;
  std::deque<proto::json> events_;
};

}  // namespace gtabot::mcp
