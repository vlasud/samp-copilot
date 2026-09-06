#include "bridge.hpp"

#include <deque>
#include <mutex>

namespace gtabot::asi {
namespace {

// If the game thread stops draining (loading screen, alt-tab, a stall) the
// queue must not grow without bound.
constexpr std::size_t kMaxTasks = 256;

std::mutex                       g_task_mutex;
std::deque<Bridge::Task>         g_tasks;
std::size_t                      g_dropped = 0;

std::mutex   g_world_mutex;
json  g_world;
std::int64_t g_world_ms = 0;

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

void Bridge::SetWorld(json world) {
  std::lock_guard<std::mutex> lock(g_world_mutex);
  g_world    = std::move(world);
  g_world_ms = NowMillis();
}

json Bridge::GetWorld(std::int64_t* age_ms) {
  std::lock_guard<std::mutex> lock(g_world_mutex);
  if (age_ms) *age_ms = g_world_ms ? NowMillis() - g_world_ms : -1;
  return g_world;
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
