#pragma once
//
// The game's collision, read rather than asked.
//
// Every call into gta_sa.exe's world functions - the ground under a point,
// a line between two points - was seen by SA-MP's client protection and
// answered with the keyboard taken away, while a module that only reads
// the game's memory is left alone. So the questions are answered here from
// the same data the game's own functions walk, without running a single
// instruction of the game's:
//
//   CWorld::ms_aSectors (0xB7D0B8, 120 x 120 x 50 m): per sector a list of
//   buildings and a list of dummies; CWorld::ms_aRepeatSectors (0xB992B8,
//   16 x 16, indexed modulo 16): vehicles, peds, objects. Each entity has a
//   model index; CModelInfo::ms_modelInfoPtrs[model]->m_pColModel holds a
//   bounding box, and its CCollisionData the spheres, boxes and triangles
//   (vertices as int16 / 128) in the entity's own space. The entity's
//   matrix, or its position and heading when it has none, takes the query
//   there.
//
// The answers are what the game would give for buildings, dummies and
// objects - and vehicles when asked - with every surface counted as solid.
// Only what is streamed in has collision data; elsewhere the answer is "no
// ground", as it was with the game's own function. Game thread only: the
// lists change under the streamer.
//
#include <string>

namespace gtabot::game {
struct Vec3;
}

namespace gtabot::game::col {

// Whether the sector table and the model table are where this build keeps
// them. Cheap; checked on the first query.
bool Ready();

// The first solid surface below (x, y) starting from z, down to a thousand
// metres under. Buildings, dummies and objects.
bool GroundBelow(float x, float y, float z, float* ground_z);

// Whether nothing solid lies on the segment from a to b. Buildings, dummies,
// objects, and vehicles when asked.
bool LineClear(const Vec3& a, const Vec3& b, bool vehicles);

// The water surface at (x, y), from the game's own data/water.dat. False
// where there is no water.
bool WaterAt(float x, float y, float* level);

// For the input line: queries, entities and primitives looked at.
std::string Line();

// Writes to the log what a ground read under this point looked at: the
// sector, the entities, the first few in detail.
void Explain(float x, float y, float z);

}  // namespace gtabot::game::col
