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
#include <cstdint>
#include <string>
#include <vector>

namespace gtabot::game {
struct Vec3;
}

namespace gtabot::game::col {

// Whether the sector table and the model table are where this build keeps
// them. Cheap; checked on the first query.
bool Ready();

// The first solid surface below (x, y) starting from z, down to a thousand
// metres under. Buildings, dummies and objects.
// Objects count by default: a character really does stand on a crate, and a
// server that builds a station platform out of them expects him to. The
// game's own ground probe leaves them out, which is what to ask for when the
// question is where the terrain is rather than what he is standing on.
bool GroundBelow(float x, float y, float z, float* ground_z,
                 bool include_objects = true);

// Whether nothing solid lies on the segment from a to b. Buildings, dummies,
// objects, and vehicles when asked.
bool LineClear(const Vec3& a, const Vec3& b, bool vehicles);

// The water surface at (x, y), from the game's own data/water.dat. False
// where there is no water.
bool WaterAt(float x, float y, float* level);

// The solid world inside a square, drawn onto a grid.
//
// Not a line here and a line there: every collision primitive of every
// streamed thing whose height overlaps a band above the floor, projected
// onto the floor and painted into cells, each grown by `inflate` so that a
// cell is marked wherever a body of that half-width would touch something.
// A bed frame at the shin, a railing post, a planter's rim - things a probe
// at knee height steps over and a character does not - are on this map
// because the whole of each thing is on it, not the few lines a probe
// happened to cast.
struct Footprint {
  float x0 = 0, y0 = 0;     // the cell (0, 0) corner, in the world
  float cell = 0.25f;
  int   side = 0;           // cells across
  // One byte a cell, row-major from (x0, y0): 1 where something solid is
  // within `inflate` of the cell's centre in the band, 0 where nothing is.
  std::vector<std::uint8_t> blocked;
  int entities = 0, primitives = 0, painted = 0;
  // Set when the cell budget ran out before everything was painted: what
  // came after the cut is missing from the square, and the caller should
  // know the picture is short rather than trust it.
  bool starved = false;
  // Sectors whose paint faulted partway - a list node or an entity that
  // could not be read. What came after the fault in that sector is missing.
  int faulted = 0;
};

// Paints the square of `radius` about (cx, cy). The band is
// [floor_z + z_lo, floor_z + z_hi]. Buildings, dummies and objects; and
// vehicles when asked - a car parked across a pavement is as solid as a
// wall for as long as it stands there, and a field that did not know of it
// sent the walker straight into one. Game thread. False when the world
// does not read.
// One thing, by where it stands.
struct Leaf { float x = 0, y = 0, z = 0; };

// `skip_here`: the things not to paint - the door leaves, which are solid to
// the painter and open to a person who walks into them.
//
// By position, one leaf at a time, never by model. A hospital's glass ward
// wall is built out of the same model as its doors, and skipping the model
// erased the whole wall: the map showed a way through and the character
// stood against the glass looking at the sea.
// One more thing standing in the way, that the pools do not hold: a player.
// Peds live in their own pool and move every frame, so they are handed in
// rather than looked up here.
struct Body { float x = 0, y = 0, z = 0, radius = 0; };

// The floor under each cell, read beforehand, so the band is judged against
// the ground a cell actually has rather than one height for the whole
// square. Without it a staircase rising through the band is painted as a
// wall - which is how a courtyard whose only way out was a staircase came
// out sealed. With it, a surface that is the ground at a cell is not an
// obstacle there, and only what stands above that ground is.
struct Floors {
  const float*        z = nullptr;      // row-major from (x0, y0)
  const std::uint8_t* known = nullptr;  // 1 where z is a reading
  int   w = 0, h = 0;
  float x0 = 0, y0 = 0, cell = 0.5f;
};

bool PaintFootprint(float cx, float cy, float floor_z, float radius, float cell,
                    float z_lo, float z_hi, float inflate,
                    const std::vector<Leaf>& skip_here,
                    const std::vector<Body>& also, Footprint* out,
                    const Floors* floors = nullptr, bool vehicles = false);

// For the input line: queries, entities and primitives looked at.
std::string Line();

// Writes to the log what a ground read under this point looked at: the
// sector, the entities, the first few in detail.
void Explain(float x, float y, float z);

}  // namespace gtabot::game::col
