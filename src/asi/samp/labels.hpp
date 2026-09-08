#pragma once
//
// The words the server hangs in the air.
//
// A role-play server says a great deal through three-dimensional text: what
// a building is, whose house this is, what to press at the barrier ahead.
// None of it goes through the chat, so something reading only the chat is
// standing in a street full of signs with its eyes shut.
//
// The client keeps them in a pool of its own, reached through CNetGame, and
// the offsets to it come from the structures BlastHack published rather than
// from a guess: guessing found the game's own string tables three times over.
//
#include <cstdint>
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::samp {

using game::Vec3;

struct Label {
  int   id = -1;
  std::string text;
  Vec3  at;
  float draw_distance = 0;
  // From wherever it was asked about. Less than nothing when the label is
  // hung on a player or a car, whose own position is where it really is.
  float away_m = 0;
  bool  behind_walls = false;
  std::uint16_t attached_to_player  = 0xFFFF;
  std::uint16_t attached_to_vehicle = 0xFFFF;
};

// Nearest first. Reading only, so it does not need the game thread.
std::vector<Label> LabelsNear(const Vec3& at, float radius, std::size_t max);

// For the status line and for saying whether the search worked at all.
std::string LabelsNote();

// The first few of whatever was found, wherever they are. For seeing what
// the table actually holds when nothing turns up nearby.
std::vector<Label> LabelsAny(const Vec3& from, std::size_t max);

}  // namespace gtabot::samp
