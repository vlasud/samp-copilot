#include "state/plan.hpp"

#include <windows.h>

#include <fstream>

#include "types.hpp"

#include <windows.h>

#include <mutex>

namespace gtabot::state {
namespace {

std::string g_task;
unsigned long long g_task_read_ms = 0;

}  // namespace

std::string TaskAsked() {
  const unsigned long long now = GetTickCount64();
  if (now - g_task_read_ms < 1000 && g_task_read_ms != 0) return g_task;
  g_task_read_ms = now;
  g_task.clear();
  std::ifstream file(gtabot::ModuleDirectory() + "bot.task");
  if (file) {
    std::string line;
    while (std::getline(file, line)) {
      while (!line.empty() && (line.back() == 13 || line.back() == 10))
        line.pop_back();
      if (line.empty()) continue;
      if (!g_task.empty()) g_task += " ";
      g_task += line;
    }
  }
  if (g_task.size() > 300) g_task.resize(300);
  return g_task;
}

namespace {

// More than this on screen is a wall of text nobody reads while a character
// is walking about.
constexpr std::size_t kMostSteps = 8;

std::mutex g_mutex;
Plan g_plan;
// When the brain last looked, and whether it has looked since it last spoke.
long long g_looked_ms = 0;
long long g_last_look_ms = 0;
bool g_looked_since = false;

}  // namespace

void NotedLook() {
  std::lock_guard<std::mutex> lock(g_mutex);
  const long long now = static_cast<long long>(GetTickCount64());
  // Only the first look after a plan starts the thinking clock: a brain that
  // looks three times before deciding was thinking through all three. But
  // every look is remembered, because "when did it last ask" is the one sign
  // of life that shows while it is still making up its mind.
  if (!g_looked_since) {
    g_looked_ms = now;
    g_looked_since = true;
  }
  g_last_look_ms = now;
}

void SetPlan(const std::string& summary, std::vector<std::string> steps,
             int doing) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const long long now = static_cast<long long>(GetTickCount64());
  g_plan.thought_ms = g_looked_since ? now - g_looked_ms : -1;
  g_looked_since = false;
  g_plan.ever = true;
  g_plan.summary = summary;
  if (steps.size() > kMostSteps) steps.resize(kMostSteps);
  g_plan.steps = std::move(steps);
  g_plan.doing = doing;
  g_plan.posted_ms = now;
}

Plan GetPlan() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Plan out = g_plan;
  out.looked_ago_ms =
      g_last_look_ms == 0
          ? -1
          : static_cast<long long>(GetTickCount64()) - g_last_look_ms;
  out.age_ms = out.posted_ms == 0
                   ? 0
                   : static_cast<long long>(GetTickCount64()) - out.posted_ms;
  return out;
}

}  // namespace gtabot::state
