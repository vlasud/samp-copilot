#include "state.hpp"

namespace gtabot::mcp {

void State::OnLinkState(bool connected) {
  std::lock_guard<std::mutex> lock(mutex_);
  link_up_ = connected;
  link_changed_ms_ = proto::NowMillis();
  if (!connected) hello_ = nullptr;
}

void State::OnMessage(const proto::Envelope& env) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (env.type == proto::msg::kHello) {
    hello_ = env.payload;
  } else if (env.type == proto::msg::kSnapshot) {
    snapshot_ = env.payload;
    snapshot_ms_ = env.ts;
    ++snapshots_seen_;
  } else if (env.type == proto::msg::kEvent) {
    events_.push_back({{"ts", env.ts}, {"payload", env.payload}});
    if (events_.size() > kMaxEvents) events_.pop_front();
  }
}

proto::json State::StatusJson() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return proto::json{
      {"link_up", link_up_},
      {"link_changed_ms", link_changed_ms_},
      {"asi", hello_.is_null() ? proto::json(nullptr) : hello_},
      {"snapshots_seen", snapshots_seen_},
      {"last_snapshot_ms", snapshot_ms_},
      {"events_buffered", events_.size()},
      {"now_ms", proto::NowMillis()},
  };
}

proto::json State::RecentEvents(std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t take = limit < events_.size() ? limit : events_.size();
  proto::json out = proto::json::array();
  for (auto it = events_.end() - static_cast<std::ptrdiff_t>(take);
       it != events_.end(); ++it) {
    out.push_back(*it);
  }
  return out;
}

proto::json State::LatestSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return proto::json{{"ts", snapshot_ms_},
                     {"snapshot", snapshot_.is_null() ? proto::json(nullptr)
                                                      : snapshot_}};
}

}  // namespace gtabot::mcp
