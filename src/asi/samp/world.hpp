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
  // Where the local id sits, verified against the pool: the local player is
  // never one of the remote slots, so a candidate that names an occupied slot
  // is the wrong field.
  std::uint32_t  local_id_at = 0;
  // How many bytes this build's std::string occupies. Confirmed by a fact, not
  // by a guess: the field right after the local player's name is a
  // CLocalPlayer*, so the width that puts a heap pointer there is the width.
  std::uint32_t  string_width = 0;
  // Derived from string_width and the declaration order, not searched for.
  // CPlayerInfo is { player*, isNPC, align, name, score, ping }.
  std::uint32_t  score_at = 0;
  std::uint32_t  ping_at  = 0;
  // True when CPlayerInfo's own shape checks out: a heap pointer where the
  // remote player belongs and a 0/1 flag where the NPC bit belongs. That says
  // the offsets are right; whether the server fills them in is separate.
  bool           confirmed = false;
  // Whether the local id names a slot that is also in the remote pool.
  bool           local_id_occupied = false;
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

// The local player's game ped, for code that needs to reason about him
// rather than report him: where he stands and which way he faces. Heading is
// the direction of the entity's forward vector, in radians, atan2 style.
struct LocalPed {
  bool           valid    = false;
  std::uintptr_t game_ped = 0;
  float          x = 0, y = 0, z = 0;
  float          heading = 0;
};
LocalPed ReadLocalPed();

// SA-MP's CLocalPlayer, as the player pool records it beside the local
// name; 0 until the layout is resolved.
std::uintptr_t LocalPlayerObject();

// Writes bot.playerinfo-dump.txt: the first occupied records, each word
// classified, and the CRemotePlayer each one points at. Game thread only.
bool DumpPlayerRecords();

}  // namespace gtabot::samp
