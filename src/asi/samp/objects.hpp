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

// One of the server's own objects, near something.
struct NearObject {
  int   id = -1;
  int   model = 0;
  Vec3  at;
  float away_m = 0;
};

// The server's objects within `radius`, nearest first. A door that opens
// when it is pushed is one of these, and so is everything else a server
// builds a room out of - which is why the walk asks what is in front of it
// before deciding that the way is shut.
std::vector<NearObject> ObjectsNear(const Vec3& at, float radius,
                                    std::size_t max);

// Whether a model is one of the game's doors. The generic ones a server
// builds a room out of run from 1491 to 1533 - gen_doorINT and gen_doorEXT -
// and 1494 is the one that stood between the character and the corridor.
bool IsDoorModel(int model);

// A pickup: the thing a server puts on the floor to be walked into. The way
// out of an interior is very often one of these, and so is every shop
// counter, job point and entrance in the game.
struct Pickup {
  int   id = -1;
  int   model = 0;
  int   type = 0;
  Vec3  at;
  float away_m = 0;
};

std::vector<Pickup> PickupsNear(const Vec3& at, float radius, std::size_t max);

// Just the doors, nearest first.
std::vector<NearObject> DoorsNear(const Vec3& at, float radius,
                                  std::size_t max);

}  // namespace gtabot::samp
