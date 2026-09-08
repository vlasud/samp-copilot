#include "samp/dialog_path.hpp"

#include <windows.h>

#include <mutex>

#include "log.hpp"
#include "samp/dialog.hpp"
#include "samp/keys.hpp"
#include "samp/talk.hpp"

namespace gtabot::samp {
namespace {

// How long a step waits for the server to send the next dialog before it
// gives up. A round trip to a Moscow server and back is a tenth of a second
// on a good day and several on a bad one; ten seconds is patient enough to
// be certain nothing is coming.
constexpr unsigned long long kWaitForNextMs = 10000;
// After answering, the dialog on screen is still the answered one for a
// frame or two. Anything read inside this window is the old one.
constexpr unsigned long long kSettleMs = 400;

std::mutex g_mutex;
std::vector<std::string> g_steps;
std::size_t g_at = 0;
bool        g_walking = false;
int         g_answered_id = -1;
unsigned long long g_answered_ms = 0;
std::string g_note = "idle";
std::vector<std::string> g_rows;

void FinishLocked(std::string why) {
  g_walking = false;
  g_note = std::move(why);
  g_steps.clear();
}

}  // namespace

void WalkDialogs(std::vector<std::string> steps) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_steps = std::move(steps);
  g_at = 0;
  g_rows.clear();
  g_answered_id = -1;
  g_answered_ms = GetTickCount64();
  if (g_steps.empty()) {
    g_walking = false;
    g_note = "nothing to walk";
    return;
  }
  g_walking = true;
  g_note = "waiting for the first dialog";
  LOG_INFO("dialogs: walking {} steps, first \"{}\"",
           static_cast<int>(g_steps.size()), g_steps.front());
}

void StopWalkingDialogs(const char* why) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_walking) LOG_INFO("dialogs: {}", why);
  FinishLocked(why);
}

void DialogPathTick() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_walking) return;
  const unsigned long long now = GetTickCount64();
  if (now - g_answered_ms < kSettleMs) return;
  // A keystroke of ours still in flight means the last answer has not landed.
  if (KeysBusy()) return;

  const Dialog dialog = CurrentDialog();
  if (!dialog.valid) return;

  if (!dialog.shown || dialog.id == g_answered_id) {
    if (now - g_answered_ms > kWaitForNextMs)
      FinishLocked(g_at == 0
                       ? "no dialog appeared to walk"
                       : "the server showed nothing after step " +
                             std::to_string(g_at));
    return;
  }

  const std::vector<std::string> rows = Rows(dialog.text);
  const std::string& want = g_steps[g_at];
  const int row = RowSaying(rows, want);
  if (row == -1) {
    g_rows = rows;
    FinishLocked("nothing on \"" + dialog.caption + "\" says \"" + want + "\"");
    LOG_WARN("dialogs: {}", g_note);
    return;
  }
  if (row == -2) {
    g_rows = rows;
    FinishLocked("more than one row says \"" + want + "\" - name it more exactly");
    LOG_WARN("dialogs: {}", g_note);
    return;
  }

  // The selection starts wherever the client left it, so it is walked to the
  // top and counted down from there - the same way a hand would.
  KeysPress(VK_UP, static_cast<int>(rows.size()));
  if (row > 0) KeysPress(VK_DOWN, row);
  KeysPress(VK_RETURN);
  g_answered_id = dialog.id;
  g_answered_ms = now;
  ++g_at;
  LOG_INFO("dialogs: step {} of {} - chose \"{}\" (row {} of \"{}\")",
           static_cast<int>(g_at), static_cast<int>(g_steps.size()), want, row,
           dialog.caption);
  if (g_at >= g_steps.size())
    FinishLocked("walked all " + std::to_string(g_at) + " steps");
}

PathStatus DialogPathGet() {
  std::lock_guard<std::mutex> lock(g_mutex);
  PathStatus out;
  out.walking = g_walking;
  out.step = g_at;
  out.steps = g_walking ? g_steps.size() : g_at;
  out.note = g_note;
  out.rows = g_rows;
  return out;
}

}  // namespace gtabot::samp
