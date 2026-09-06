#include "bridge.hpp"

#include <deque>
#include <mutex>

namespace gtabot::asi {
namespace {

// If the game thread stops draining (loading screen, alt-tab, a stall) the
// queues must not grow without bound.
constexpr std::size_t kMaxTasks   = 256;
constexpr std::size_t kMaxOutbox  = 512;

std::mutex                       g_task_mutex;
std::deque<Bridge::Task>         g_tasks;
std::size_t                      g_dropped = 0;

std::mutex                       g_outbox_mutex;
std::deque<proto::Envelope>      g_outbox;

}  // namespace

void Bridge::PostToGameThread(Task task) {
  std::lock_guard<std::mutex> lock(g_task_mutex);
  if (g_tasks.size() >= kMaxTasks) {
    ++g_dropped;
    return;
  }
  g_tasks.push_back(std::move(task));
}

void Bridge::RunPending(std::size_t max_tasks) {
  for (std::size_t i = 0; i < max_tasks; ++i) {
    Task task;
    {
      std::lock_guard<std::mutex> lock(g_task_mutex);
      if (g_tasks.empty()) return;
      task = std::move(g_tasks.front());
      g_tasks.pop_front();
    }
    // The frame hook already seals exceptions, but a task must not be able to
    // abort the ones queued behind it.
    try {
      task();
    } catch (...) {
    }
  }
}

void Bridge::Publish(proto::Envelope envelope) {
  std::lock_guard<std::mutex> lock(g_outbox_mutex);
  if (g_outbox.size() >= kMaxOutbox) g_outbox.pop_front();
  g_outbox.push_back(std::move(envelope));
}

std::vector<proto::Envelope> Bridge::DrainOutbox() {
  std::lock_guard<std::mutex> lock(g_outbox_mutex);
  std::vector<proto::Envelope> out(std::make_move_iterator(g_outbox.begin()),
                                   std::make_move_iterator(g_outbox.end()));
  g_outbox.clear();
  return out;
}

std::size_t Bridge::pending_tasks() {
  std::lock_guard<std::mutex> lock(g_task_mutex);
  return g_tasks.size();
}

std::size_t Bridge::dropped_tasks() {
  std::lock_guard<std::mutex> lock(g_task_mutex);
  return g_dropped;
}

}  // namespace gtabot::asi
