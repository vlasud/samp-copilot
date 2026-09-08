#pragma once
//
// The words the server paints onto things.
//
// A server can put text on any face of any object it has created, and many
// do: the sign over a shop, the number on a house, the notice on a barrier
// saying which key opens it. None of it is chat, none of it is a label
// hanging in the air, and none of it reaches anything that reads only those.
//
// Each object carries up to sixteen materials, and a material is either a
// texture or a piece of text. The text ones are what this returns.
//
#include <cstdint>
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::samp {

using game::Vec3;

struct ObjectText {
  int   object_id = -1;
  int   material = -1;
  int   model = 0;
  std::string text;
  std::string font;
  Vec3  at;
  float away_m = 0;
};

// The painted text on the server's objects within `radius`, nearest first.
// Reading only, so any thread will do.
std::vector<ObjectText> ObjectTextsNear(const Vec3& at, float radius,
                                        std::size_t max);

std::string ObjectTextsNote();

}  // namespace gtabot::samp
