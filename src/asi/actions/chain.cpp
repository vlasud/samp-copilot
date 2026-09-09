#include "actions/chain.hpp"

#include <windows.h>

#include <cmath>
#include <mutex>

#include "actions/travel.hpp"
#include "actions/walker.hpp"
#include "game/bindings.hpp"
#include "log.hpp"
#include "types.hpp"
#include "samp/chat.hpp"
#include "samp/dialog.hpp"
#include "samp/keys.hpp"
#include "samp/talk.hpp"
#include "samp/world.hpp"

namespace gtabot::act {
namespace {

// How long a step may take before it is called blocked. A walk across a
// hospital is a minute; anything longer is not going to happen.
constexpr unsigned long long kStepLimitMs = 90000;
// After a key or a line of chat, a moment for the game to notice.
constexpr unsigned long long kAfterKeyMs = 700;
constexpr unsigned long long kAfterSayMs = 900;

std::mutex g_mutex;
std::vector<Step> g_steps;
StopWhen g_when;
bool        g_running = false;
std::size_t g_at = 0;
bool        g_started_step = false;
unsigned long long g_step_ms = 0;
unsigned long long g_began_ms = 0;
std::string g_stopped_by = "";
std::string g_note = "idle";
// What the world looked like when the chain started, for noticing change.
// Health and the character's own name come from the world snapshot, which is
// dear enough to ask for once a second rather than every frame.
float g_health_was = -1.0f;
float g_health_now = -1.0f;
std::string g_my_name;
unsigned long long g_read_world_ms = 0;

void RefreshWorld(unsigned long long now, bool force) {
  if (!force && now - g_read_world_ms < 1000) return;
  g_read_world_ms = now;
  const json world = samp::ReadWorld();
  if (!world.is_object() || !world.contains("self")) return;
  const json& self = world["self"];
  g_my_name = self.value("name", g_my_name);
  const float health = self.value("health", -1.0f);
  if (health >= 0) g_health_now = health;
}
bool  g_had_dialog = false;
std::string g_last_chat;

std::string Describe(const Step& step) {
  if (step.kind == "go") {
    char text[64];
    std::snprintf(text, sizeof(text), "go to (%.0f, %.0f)", step.x, step.y);
    return text;
  }
  if (step.kind == "press") return "press " + step.key;
  if (step.kind == "say") return "say \"" + step.text + "\"";
  if (step.kind == "answer") return "answer the dialog";
  if (step.kind == "wait") return "wait";
  return step.kind;
}

void FinishLocked(const char* why, std::string note) {
  g_running = false;
  g_stopped_by = why;
  g_note = std::move(note);
  Stop("the chain ended");
  CancelTravel("the chain ended");
  LOG_INFO("chain: {} after {} of {} steps - {}", why, static_cast<int>(g_at),
           static_cast<int>(g_steps.size()), g_note);
}

// Has the world done something the brain should hear about?
const char* SomethingHappened(bool answering) {
  const samp::Dialog dialog = samp::CurrentDialog();
  if (g_when.on_dialog && dialog.valid && dialog.shown && !answering &&
      !g_had_dialog)
    return "dialog";
  g_had_dialog = dialog.valid && dialog.shown;

  if (g_when.on_hurt && g_health_was >= 0 && g_health_now >= 0) {
    if (g_health_was - g_health_now >= g_when.hurt_by) return "hurt";
    if (g_health_now > g_health_was) g_health_was = g_health_now;
  }
  return nullptr;
}

}  // namespace

void RunChain(std::vector<Step> steps, const StopWhen& when) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Stop("replaced by a chain");
  CancelTravel("replaced by a chain");
  g_steps = std::move(steps);
  g_when = when;
  g_at = 0;
  g_started_step = false;
  g_running = !g_steps.empty();
  g_began_ms = GetTickCount64();
  g_step_ms = g_began_ms;
  g_stopped_by.clear();
  g_note = g_running ? "starting" : "nothing to do";
  const samp::Dialog dialog = samp::CurrentDialog();
  g_had_dialog = dialog.valid && dialog.shown;
  RefreshWorld(g_began_ms, true);
  g_health_was = g_health_now;
  g_last_chat.clear();
  if (g_running)
    LOG_INFO("chain: {} steps, first {}", static_cast<int>(g_steps.size()),
             Describe(g_steps.front()));
}

void StopChain(const char* why) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_running) FinishLocked("cancelled", why);
}

void ChainTick() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_running) return;
  const unsigned long long now = GetTickCount64();
  const samp::LocalPed self = samp::ReadLocalPed();
  if (!self.valid) return;
  RefreshWorld(now, false);

  if (g_at >= g_steps.size()) {
    FinishLocked("done", "every step done");
    return;
  }
  const Step& step = g_steps[g_at];
  const bool answering = step.kind == "answer";

  if (const char* what = SomethingHappened(answering)) {
    FinishLocked(what, std::string("stopped at step ") +
                           std::to_string(static_cast<int>(g_at) + 1) + ": " +
                           Describe(step));
    return;
  }

  // Somebody used his name. Read only what is new since the chain started.
  if (g_when.on_spoken_to) {
    const json chat = samp::ReadChat(6);
    const json lines = chat.value("lines", json::array());
    if (!lines.empty()) {
      const std::string newest = lines.back().value("text", std::string{});
      if (g_last_chat.empty()) {
        g_last_chat = newest;
      } else if (newest != g_last_chat) {
        g_last_chat = newest;
        const samp::TalkLine said = samp::Classify(
            newest, lines.back().value("from", std::string{}), g_my_name);
        if (said.to_me) {
          FinishLocked("spoken_to", said.speaker.empty()
                                        ? said.plain
                                        : said.speaker + ": " + said.plain);
          return;
        }
      }
    }
  }

  if (now - g_step_ms > kStepLimitMs) {
    FinishLocked("blocked", "step " + std::to_string(static_cast<int>(g_at) + 1) +
                                " took too long: " + Describe(step));
    return;
  }

  const auto next = [&]() {
    ++g_at;
    g_started_step = false;
    g_step_ms = now;
  };

  if (step.kind == "go") {
    const float away = std::sqrt((self.x - step.x) * (self.x - step.x) +
                                 (self.y - step.y) * (self.y - step.y));
    if (away <= step.stop_within + 1.2f) { next(); return; }
    if (!g_started_step) {
      g_started_step = true;
      TravelTo(Vec3{step.x, step.y, self.z}, false, step.stop_within);
      g_note = Describe(step);
      return;
    }
    const TravelStatus trip = TravelGet();
    if (!trip.travelling) {
      // It stopped without arriving: let the journey have another go until
      // the step's own limit runs out.
      g_started_step = false;
    }
    return;
  }

  if (step.kind == "press") {
    if (!g_started_step) {
      g_started_step = true;
      int vk = 0;
      if (step.key == "walk" || step.key == "use")
        vk = game::KeyForAction(game::kPedWalk, VK_LMENU);
      else if (step.key == "enter_exit")
        vk = game::KeyForAction(game::kVehicleEnterExit, VK_RETURN);
      else if (step.key == "jump")
        vk = game::KeyForAction(game::kJumping, VK_SPACE);
      else
        vk = game::KeyFromName(step.key);
      if (vk == 0) { FinishLocked("blocked", "no key called " + step.key); return; }
      samp::KeysPressFor(vk, (step.ms > 0 ? step.ms : 350) / 16);
      g_note = Describe(step);
      g_step_ms = now;
      return;
    }
    if (!samp::KeysBusy() && now - g_step_ms > kAfterKeyMs) next();
    return;
  }

  if (step.kind == "say") {
    if (!g_started_step) {
      g_started_step = true;
      samp::KeysPress('T');
      samp::KeysType(step.text);
      samp::KeysPress(VK_RETURN);
      g_note = Describe(step);
      g_step_ms = now;
      return;
    }
    if (!samp::KeysBusy() && now - g_step_ms > kAfterSayMs) next();
    return;
  }

  if (step.kind == "answer") {
    if (!g_started_step) {
      const samp::Dialog dialog = samp::CurrentDialog();
      if (!dialog.valid || !dialog.shown) {
        // Nothing to answer yet; wait for it within the step's own limit.
        g_note = "waiting for a dialog to answer";
        return;
      }
      g_started_step = true;
      int row = step.item;
      if (row < 0 && !step.choose.empty()) {
        const std::vector<std::string> rows = samp::Rows(dialog.text);
        row = samp::RowSaying(rows, step.choose);
        if (row < 0) {
          FinishLocked("blocked", "no row says \"" + step.choose + "\"");
          return;
        }
      }
      if (row >= 0) {
        int count = 1;
        for (const char c : dialog.text) if (c == 0x0A) ++count;
        samp::KeysPress(VK_UP, count);
        if (row > 0) samp::KeysPress(VK_DOWN, row);
      }
      if (!step.text.empty()) samp::KeysType(step.text);
      samp::KeysPress(step.button == 2 ? VK_ESCAPE : VK_RETURN);
      g_note = Describe(step);
      g_step_ms = now;
      g_had_dialog = false;   // this one was asked for
      return;
    }
    if (!samp::KeysBusy() && now - g_step_ms > kAfterKeyMs) next();
    return;
  }

  if (step.kind == "wait") {
    if (!g_started_step) {
      g_started_step = true;
      g_step_ms = now;
      g_note = "waiting";
      return;
    }
    if (now - g_step_ms >= static_cast<unsigned long long>(step.ms)) next();
    return;
  }

  FinishLocked("blocked", "no step called " + step.kind);
}

ChainStatus ChainGet() {
  std::lock_guard<std::mutex> lock(g_mutex);
  ChainStatus out;
  out.running = g_running;
  out.at = g_at;
  out.steps = g_steps.size();
  out.doing = g_running && g_at < g_steps.size() ? Describe(g_steps[g_at]) : "";
  out.stopped_by = g_stopped_by;
  out.note = g_note;
  out.ran_ms = g_began_ms == 0
                   ? 0
                   : static_cast<long long>(GetTickCount64() - g_began_ms);
  return out;
}

}  // namespace gtabot::act
