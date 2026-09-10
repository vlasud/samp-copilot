#pragma once
//
// A field, written to a file and read back without the game.
//
// Every real fix in this navigation work came from one number, and every
// number cost twenty-five minutes: close the game, copy the mod in, wait for
// the login, wait for the spawn, walk him there, watch. That is the whole of
// why three days went the way they did - not the algorithms, the looking.
//
// So the field writes down what it saw. A case is the grid exactly as the
// paint left it - before the clearance, before the search, so the rules
// under test are not baked into the evidence - together with where he stood,
// where he was sent, and what the mod made of it at the time. It is written
// whenever a journey goes wrong, so a bad episode leaves evidence of itself
// without anybody having to be watching.
//
// Read back by tools/fieldcase, which runs the clearance and the search over
// it again with whatever rules are in the build now, and says what came out.
// Fifty of those in a few seconds is the loop this needed from the start.
//
#include <cstdint>
#include <string>
#include <vector>

#include "nav/grid.hpp"

namespace gtabot::nav {

struct FieldCase {
  // The grid as the paint left it: blocked and ground and known are real,
  // clear is not filled in yet - the clearance is what a replay re-runs.
  Grid  grid;
  Vec3  from{0, 0, 0};
  Vec3  to{0, 0, 0};
  float ref_z = 0;
  int   stride = 4;
  // What the mod made of it when this was written, for comparison.
  std::string note;
  std::vector<Vec3> route;
  std::string why;         // why it was written down: "shut in", "no route", asked for
};

// The same thing in the form the field keeps while a plan is in flight: the
// ground as centimetres from the reference height in sixteen bits, and known
// and blocked sharing a byte. Three bytes a cell rather than eight, because
// it is taken for every plan and written out only for the ones that went
// wrong - and by then the clearance has already changed the grid it came
// from, so it has to be a copy.
struct CaseShot {
  int   W = 0, H = 0;
  float cell = 0, x0 = 0, y0 = 0;
  float ref_z = 0;
  std::vector<std::uint8_t> flags;   // bits 0-1 known, bit 2 solid
  std::vector<std::int16_t> floor;   // centimetres from ref_z
  bool empty() const { return W <= 0 || H <= 0 || flags.empty(); }
  void Take(const Grid& g, float reference);
};

bool SaveCase(const FieldCase& one, const std::string& path);
bool SaveShot(const CaseShot& shot, const FieldCase& context,
              const std::string& path);
bool LoadCase(const std::string& path, FieldCase* out);

}  // namespace gtabot::nav
