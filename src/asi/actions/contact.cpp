#include "actions/contact.hpp"

#include <windows.h>

#include <cmath>

#include "log.hpp"

namespace gtabot::act {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kHalfPi = 1.5707963f;

// How often the question is asked. Long enough that a step is a step and not
// the jitter of one frame's physics.
constexpr unsigned long long kWindowMs = 260;
// A character on foot covers about four metres a second at a walk. Getting
// less than this fraction of what the window should have carried him is
// being against something.
constexpr float kExpectedSpeed = 3.6f;
constexpr float kStoppedFraction = 0.35f;
// While sliding, this much progress along the tangent says the wall is being
// followed rather than fought.
constexpr float kSlidingFraction = 0.5f;
// How long to keep sliding before trying the wanted way again. A door frame
// is half a metre; a ward wall is fifteen.
constexpr unsigned long long kSlideAtLeastMs = 500;
constexpr unsigned long long kSlideAtMostMs = 6000;
// Turning back onto the wanted heading is allowed when it points away from
// the thing he is against - more than this off the way that was blocked.
constexpr float kClearOfBlocked = 1.05f;   // sixty degrees
// Sliding into the same wall from the other side: the side is flipped only
// after this many windows of getting nowhere along the tangent.
constexpr int kStuckWindowsBeforeFlip = 3;

Vec3  g_was{};
bool  g_had = false;
unsigned long long g_window_ms = 0;
float g_wanted_at_window = 0;

Contact g_state;
int   g_stuck_windows = 0;
int   g_last_side = 1;

float Normalise(float a) {
  while (a > kPi) a -= 2.0f * kPi;
  while (a < -kPi) a += 2.0f * kPi;
  return a;
}

float Distance2D(const Vec3& a, const Vec3& b) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

void ContactReset() {
  g_had = false;
  g_window_ms = 0;
  g_stuck_windows = 0;
  g_state = Contact{};
}

float ContactSteer(const Vec3& here, float wanted, float speed_wanted,
                   unsigned long long now) {
  if (!g_had) {
    g_had = true;
    g_was = here;
    g_window_ms = now;
    g_wanted_at_window = wanted;
    g_state.slide_heading = wanted;
    return wanted;
  }

  // A real change of intention - a corner turned, a new leg - is not a wall
  // to be followed. Let go of the one being followed and look again.
  if (g_state.side != 0 &&
      std::fabs(Normalise(wanted - g_state.blocked_heading)) > kClearOfBlocked &&
      now - g_state.since_ms > kSlideAtLeastMs) {
    g_state.side = 0;
    g_stuck_windows = 0;
  }

  if (now - g_window_ms >= kWindowMs) {
    const float went = Distance2D(here, g_was);
    const float should_have =
        kExpectedSpeed * speed_wanted * (now - g_window_ms) / 1000.0f;
    const bool moving = should_have > 0.01f &&
                        went >= should_have * (g_state.side == 0 ? kStoppedFraction
                                                                 : kSlidingFraction);

    if (!moving) {
      if (g_state.side == 0) {
        // The first touch. Which way to turn is decided once, and the same
        // way as last time when there is nothing to choose between them: a
        // wall followed consistently is a wall that ends.
        g_state.side = g_last_side;
        g_state.blocked_heading = g_wanted_at_window;
        g_state.since_ms = now;
        g_state.touching = true;
        ++g_state.slides;
        g_stuck_windows = 0;
        LOG_INFO("contact: something in the way going {:.0f} degrees - "
                 "following it on the {}", g_state.blocked_heading * 57.2957795f,
                 g_state.side > 0 ? "left" : "right");
      } else if (++g_stuck_windows >= kStuckWindowsBeforeFlip) {
        // Following it that way is getting nowhere either: the other way.
        g_state.side = -g_state.side;
        g_last_side = g_state.side;
        g_state.since_ms = now;
        g_stuck_windows = 0;
        LOG_INFO("contact: no way along it that side - following it on the {}",
                 g_state.side > 0 ? "left" : "right");
      }
    } else if (g_state.side != 0) {
      g_stuck_windows = 0;
      // Moving along the wall. Try the wanted way again once the shoulder is
      // past whatever it was - which is what having moved along it means.
      if (now - g_state.since_ms > kSlideAtLeastMs) {
        g_state.side = 0;
        g_state.touching = false;
      }
    } else {
      g_state.touching = false;
    }

    if (g_state.side != 0 && now - g_state.since_ms > kSlideAtMostMs) {
      // Long enough. Whatever this is, following it is not working.
      g_state.side = 0;
      g_state.touching = false;
    }

    g_was = here;
    g_window_ms = now;
    g_wanted_at_window = wanted;
  }

  if (g_state.side == 0) {
    g_state.slide_heading = wanted;
    return wanted;
  }
  // Along the thing: a right angle off the way that was stopped, leaning a
  // little toward where he actually wants to go so he peels off as soon as
  // the wall lets him.
  const float tangent =
      Normalise(g_state.blocked_heading + g_state.side * kHalfPi);
  const float toward = Normalise(wanted - tangent);
  g_state.slide_heading = Normalise(tangent + toward * 0.25f);
  return g_state.slide_heading;
}

Contact ContactGet() { return g_state; }

}  // namespace gtabot::act
