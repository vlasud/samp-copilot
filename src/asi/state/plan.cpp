#include "state/plan.hpp"

#include <windows.h>

#include <mutex>

namespace gtabot::state {
namespace {

// More than this on screen is a wall of text nobody reads while a character
// is walking about.
constexpr std::size_t kMostSteps = 8;

std::mutex g_mutex;
Plan g_plan;
// When the brain last looked, and whether it has looked since it last spoke.
long long g_looked_ms = 0;
bool g_looked_since = false;

}  // namespace

void NotedLook() {
  std::lock_guard<std::mutex> lock(g_mutex);
  // Only the first look after a plan starts the clock: a brain that looks
  // three times before deciding was thinking through all three.
  if (!g_looked_since) {
    g_looked_ms = static_cast<long long>(GetTickCount64());
    g_looked_since = true;
  }
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
  out.age_ms = out.posted_ms == 0
                   ? 0
                   : static_cast<long long>(GetTickCount64()) - out.posted_ms;
  return out;
}

}  // namespace gtabot::state
