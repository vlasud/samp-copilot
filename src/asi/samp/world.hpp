#pragma once
//
// Reads SA-MP's player pool.
//
// The field order comes from the public SAMP-API headers for 0.3.7-R1; the
// byte offsets do not. Working those out by hand from a class declaration
// means one wrong assumption about padding produces plausible-looking
// nonsense, so each one is either anchored to something we can check or found
// by the shape of the data:
//
//   - CNetGame is at *(samp.dll + 0x21A0F8), and it is the right pointer only
//     if the host address at +0x20 matches the -h the launcher passed.
//   - CNetGame::Pools is nine consecutive heap pointers, found by that shape
//     rather than by a computed offset.
//   - CPlayerPool holds 1004 CPlayerInfo pointers followed by 1004 booleans,
//     which is a signature nothing else in the structure matches.
//   - std::string has two plausible MSVC layouts; the one that yields a
//     readable local player name is the one this client was built with.
//
// If any of that fails to line up, nothing is reported rather than something
// wrong, and `note` says where it stopped.
//
#include <cstdint>
#include <string>

#include "types.hpp"

namespace gtabot::samp {

struct Layout {
  bool           valid = false;
  std::uintptr_t net_game    = 0;
  std::uintptr_t pools       = 0;
  std::uintptr_t player_pool = 0;
  // Offsets inside CPlayerPool, established from the data.
  std::uint32_t  object_array    = 0;
  std::uint32_t  not_empty_array = 0;
  // 0 or 1: which MSVC std::string layout this build uses.
  int            string_variant = -1;
  // Where the local player's own name sits inside CPlayerPool. Found rather
  // than computed, and optional: the pool is worth reading without it.
  std::uint32_t  local_name = 0;
  // The host address read out of CNetGame - the proof the root pointer is real.
  std::string    host;
  std::string    note;
};

// Resolved once and cached. Game thread only.
const Layout& ResolveLayout();

// Re-runs resolution from scratch, e.g. after reconnecting.
void ForgetLayout();

// The local player and everyone in the pool. Game thread only.
json ReadWorld();

}  // namespace gtabot::samp
