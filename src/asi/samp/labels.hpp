#pragma once
//
// The words the server hangs in the air.
//
// A role-play server says a great deal through three-dimensional text: what
// a building is, whose house this is, what to press at the barrier ahead.
// None of it goes through the chat, so something reading only the chat is
// standing in a street full of signs with its eyes shut.
//
// Where the client keeps them is not documented and differs between builds,
// so it is not assumed: the table is found by its shape. An entry is a
// pointer to a string and a position in the world, laid end to end, and a
// run of those is what a table of them looks like and very little else does.
//
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::samp {

using game::Vec3;

struct Label {
  std::string text;
  Vec3  at;
  float draw_distance = 0;
  float away_m = 0;      // from wherever it was asked about
};

// Game thread. Nearest first. Empty until the table has been found, which
// happens on the first call and takes a moment.
std::vector<Label> LabelsNear(const Vec3& at, float radius, std::size_t max);

// For the status line and for saying whether the search worked at all.
std::string LabelsNote();

// The first few of whatever was found, wherever they are. For seeing what
// the table actually holds when nothing turns up nearby.
std::vector<Label> LabelsAny(const Vec3& from, std::size_t max);

}  // namespace gtabot::samp
