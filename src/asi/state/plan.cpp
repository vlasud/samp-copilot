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

}  // namespace

void SetPlan(const std::string& summary, std::vector<std::string> steps,
             int doing) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_plan.summary = summary;
  if (steps.size() > kMostSteps) steps.resize(kMostSteps);
  g_plan.steps = std::move(steps);
  g_plan.doing = doing;
  g_plan.posted_ms = static_cast<long long>(GetTickCount64());
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
