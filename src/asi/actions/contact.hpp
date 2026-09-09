#pragma once
//
// Feeling what he walked into, and sliding along it.
//
// Every attempt so far predicted the way ahead: cast lines, read collision
// models, paint the room, decide in advance what he would fit through. Each
// prediction was wrong somewhere - a bed frame below the lowest line, a glass
// wall built out of a door's model, a doorway sealed by its own clearance -
// and each time the character stood grinding against something the map said
// was not there.
//
// This does not predict anything. He is walking, so the game is already
// answering the only question that matters, every frame: how far did he
// actually get. Aim him somewhere, compare what he was asked to do with what
// he did, and the difference is the obstacle - its direction, without a
// single line cast or a byte of collision read.
//
// What to do about it is the oldest answer in robot navigation. Turn along
// the thing, keep turning the same way, and walk. A wall followed
// consistently either ends or comes back to where it started, so a room with
// a way out is always left; and where the geometry is something nobody
// modelled - a railing, a plant pot, another player - it works exactly the
// same, because it never asked what the thing was.
//
#include <string>

#include "game/world_query.hpp"

namespace gtabot::act {

using game::Vec3;

struct Contact {
  bool  touching = false;     // he is against something now
  float blocked_heading = 0;  // the way he was trying to go when it stopped him
  int   side = 0;             // +1 slide left, -1 slide right, 0 not sliding
  float slide_heading = 0;    // the way to go while sliding
  int   slides = 0;           // how many times he has had to slide this walk
  unsigned long long since_ms = 0;
};

// Called every frame while walking, with where he is and the heading he is
// being sent along. Returns the heading to actually walk - the wanted one
// while the way is open, a tangent along whatever he is against while it is
// not.
float ContactSteer(const Vec3& here, float wanted, float speed_wanted,
                   unsigned long long now);

// Forget the wall being followed: a new walk, or a new leg after a real
// turn. Slides are counted from here.
void ContactReset();

Contact ContactGet();

}  // namespace gtabot::act
