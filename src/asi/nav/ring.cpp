#include "nav/ring.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include "game/peds.hpp"

namespace gtabot::nav {
namespace {

constexpr float kPi = 3.14159265f;
// Sixteen ways round: a spoke every twenty-two degrees, which is finer than
// a person changes direction and coarse enough to cost nothing.
constexpr int   kSpokes = 16;
// The lines are cast at the knee and at the chest, off the floor under his
// feet, and swept at the width of his shoulders - the same body the room map
// is drawn for.
constexpr float kKnee  = 0.45f;
constexpr float kChest = 1.20f;
constexpr float kBodyRadius = 0.34f;
// How near a person has to be to count as being in the way.
constexpr float kPersonRadius = 0.45f;
// The ring is not a navigator and must never behave like one. It says
// nothing at all until the way ahead is nearly gone, and then it turns by as
// little as will clear - the first spoke, counting outward from where he
// meant to go, with room to walk. Steering at the freest direction instead
// pulled him off the route in every corridor: the freest way out of a
// corridor is back down it.
constexpr float kTooClose = 1.2f;      // less than this ahead and he must turn
constexpr float kEnough   = 1.8f;      // and this much is enough to turn into
constexpr float kMostTurn = 1.75f;     // a hundred degrees, no more

std::string g_note = "not looked round yet";

float Normalise(float a) {
  while (a > kPi) a -= 2.0f * kPi;
  while (a < -kPi) a += 2.0f * kPi;
  return a;
}

// How far along a heading he can go before something is in the way: the
// body, swept, against the collision pools. Halved four times, so within
// about a sixteenth of the reach.
float FreeAlong(const Vec3& here, float floor_z, float heading, float reach) {
  const float sx = -std::sin(heading) * kBodyRadius;
  const float sy =  std::cos(heading) * kBodyRadius;
  const float dx = std::cos(heading), dy = std::sin(heading);
  const float heights[2] = {kKnee, kChest};
  const float sides[3] = {0.0f, 1.0f, -1.0f};
  const auto clear_to = [&](float along) {
    for (const float side : sides)
      for (const float h : heights) {
        const Vec3 a{here.x + sx * side, here.y + sy * side, floor_z + h};
        const Vec3 b{here.x + dx * along + sx * side,
                     here.y + dy * along + sy * side, floor_z + h};
        if (!game::LineClear(a, b, /*include_vehicles=*/false)) return false;
      }
    return true;
  };
  if (clear_to(reach)) return reach;
  float low = 0.0f, high = reach;
  for (int i = 0; i < 4; ++i) {
    const float mid = (low + high) * 0.5f;
    if (clear_to(mid)) low = mid; else high = mid;
  }
  return low;
}

}  // namespace

Ring LookRound(const Vec3& here, float wanted, float reach) {
  Ring ring;
  if (!game::CallsTrusted()) {
    g_note = "the world does not read yet";
    return ring;
  }
  float floor_z = here.z - 1.0f;
  float ground = 0;
  if (game::GroundBelow(Vec3{here.x, here.y, here.z + 1.2f}, &ground) &&
      std::fabs(ground - (here.z - 1.0f)) < 2.0f)
    floor_z = ground;

  // Whoever is standing about, from the ped pool. A person is not in any
  // collision pool and is the obstacle most likely to be in a doorway.
  const std::vector<game::Ped> people = game::PedsNear(here, reach + 2.0f, 16);
  const auto person_in_the_way = [&](float heading, float along) {
    const float px = here.x + std::cos(heading) * along;
    const float py = here.y + std::sin(heading) * along;
    for (const game::Ped& who : people) {
      if (std::fabs(who.position.z - here.z) > 2.0f) continue;
      const float dx = who.position.x - px, dy = who.position.y - py;
      if (dx * dx + dy * dy < (kPersonRadius + kBodyRadius) *
                              (kPersonRadius + kBodyRadius))
        return true;
    }
    return false;
  };

  const auto free_along = [&](float heading) {
    float along = FreeAlong(here, floor_z, heading, reach);
    for (float at = 0.5f; at <= along; at += 0.5f)
      if (person_in_the_way(heading, at)) { along = at - 0.5f; break; }
    return along;
  };

  const float free_ahead = free_along(wanted);
  ring.ok = true;
  ring.spokes = kSpokes;
  ring.free_ahead_m = free_ahead;
  ring.steer = wanted;
  if (free_ahead >= kTooClose) {
    char open[96];
    std::snprintf(open, sizeof(open), "%.1f m ahead, walking on", free_ahead);
    g_note = open;
    return ring;
  }

  // Outward from where he meant to go, and the first with room wins.
  for (int i = 1; i < kSpokes; ++i) {
    const int step = (i + 1) / 2;
    const float away = (i % 2 == 1 ? 1.0f : -1.0f) * step * (2.0f * kPi / kSpokes);
    if (std::fabs(away) > kMostTurn) continue;
    const float heading = Normalise(wanted + away);
    if (free_along(heading) < kEnough) continue;
    ring.steer = heading;
    ring.turned = true;
    ring.turned_by_deg = away * 57.2957795f;
    break;
  }
  char note[160];
  std::snprintf(note, sizeof(note), "%.1f m ahead; %s", free_ahead,
                ring.turned ? "turning" : "nothing better all the way round");
  g_note = note;
  return ring;
}

std::string RingNote() { return g_note; }

}  // namespace gtabot::nav
