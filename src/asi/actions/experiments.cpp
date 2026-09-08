#include "actions/experiments.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "actions/travel.hpp"
#include "actions/walker.hpp"
#include "game/world_query.hpp"
#include "log.hpp"
#include "samp/input_state.hpp"
#include "samp/world.hpp"

namespace gtabot::act {
namespace {

enum class Kind { kNone, kKeys, kWorld };

Kind g_kind = Kind::kNone;
unsigned long long g_started_ms = 0;
unsigned long long g_until_ms = 0;
unsigned long long g_calls = 0;
unsigned long long g_key_events_at_start = 0;
bool g_was_enabled = false;
bool g_trusted_said = false;
char g_line[64] = "";

const char* Name(Kind kind) {
  return kind == Kind::kKeys ? "keys through the system, no calls into the game"
                             : "world read from the game's data, no keys";
}

void Finish(const char* how) {
  const unsigned long long now = GetTickCount64();
  LOG_WARN("experiment: {} - {} after {} s ({} calls into the game, {} key events)",
           Name(g_kind), how, (now - g_started_ms) / 1000, g_calls,
           KeyEventsSent() - g_key_events_at_start);
  if (g_kind == Kind::kKeys) HoldTestKeys(false);
  if (g_kind == Kind::kWorld && !g_was_enabled) game::SetEnabled(false);
  g_kind = Kind::kNone;
  g_line[0] = '\0';
}

void Start(Kind kind, unsigned seconds) {
  if (g_kind != Kind::kNone) Finish("replaced");
  Stop("an experiment starts");
  CancelTravel("an experiment starts");
  g_kind = kind;
  g_started_ms = GetTickCount64();
  g_until_ms = g_started_ms + seconds * 1000ULL;
  g_calls = 0;
  g_key_events_at_start = KeyEventsSent();
  g_trusted_said = false;
  if (kind == Kind::kWorld) {
    g_was_enabled = game::Enabled();
    game::SetEnabled(true);
  } else {
    HoldTestKeys(true);
  }
  LOG_WARN("experiment: {} - started, {} s", Name(kind), seconds);
}

// The walker's probe, without the walk: the ground under eight points
// round him and seven whiskers at three heights.
void ProbeAround(const samp::LocalPed& self) {
  const Vec3 here{self.x, self.y, self.z};
  for (int i = 0; i < 8; ++i) {
    const float a = static_cast<float>(i) * 0.7854f;
    float ground = 0;
    game::GroundBelow(Vec3{here.x + std::cos(a) * 2.0f, here.y + std::sin(a) * 2.0f,
                           here.z + 1.5f}, &ground);
    ++g_calls;
  }
  const float heights[3] = {-0.5f, 0.0f, 0.4f};
  for (int w = 0; w < 7; ++w) {
    const float a = static_cast<float>(w - 3) * 0.5f;
    for (float h : heights) {
      const Vec3 from{here.x, here.y, here.z + h};
      const Vec3 to{here.x + std::cos(a) * 4.0f, here.y + std::sin(a) * 4.0f, here.z + h};
      game::LineClear(from, to, false);
      ++g_calls;
    }
  }
}

}  // namespace

void StartKeyRun(unsigned seconds) { Start(Kind::kKeys, seconds); }
void StartWorldCalls(unsigned seconds) { Start(Kind::kWorld, seconds); }

void StopExperiments() {
  if (g_kind != Kind::kNone) Finish("stopped from the menu");
}

bool ExperimentRunning() { return g_kind != Kind::kNone; }

const char* ExperimentLine() { return g_line; }

void ExperimentTick() {
  if (g_kind == Kind::kNone) return;
  const unsigned long long now = GetTickCount64();
  const char* why = "";
  if (samp::InputLegitimatelyOff(&why) && std::strstr(why, "taken the keyboard") != nullptr) {
    Finish("the keyboard was TAKEN");
    return;
  }
  if (now >= g_until_ms) {
    Finish("finished, the keyboard was not taken");
    return;
  }
  std::snprintf(g_line, sizeof(g_line), "%s %llu с",
                g_kind == Kind::kKeys ? "бег" : "запросы",
                static_cast<unsigned long long>((g_until_ms - now + 999) / 1000));

  if (g_kind == Kind::kWorld) {
    const samp::LocalPed self = samp::ReadLocalPed();
    if (!self.valid) return;
    if (!game::CallsTrusted()) {
      const Vec3 here{self.x, self.y, self.z};
      const char* note = "";
      game::SelfCheck(here, &note);
      game::SelfCheckLineOfSight(here, &note);
      g_calls += 3;
      return;
    }
    if (!g_trusted_said) {
      g_trusted_said = true;
      LOG_INFO("experiment: game calls verified - probing every frame from now");
    }
    ProbeAround(self);
  }
}

}  // namespace gtabot::act
