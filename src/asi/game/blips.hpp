#pragma once
//
// Every mark on the radar, whoever put it there.
//
// The waypoint a player clicks onto the map is one of these, and `MapMarker`
// reads that one by its handle. But a server marks places too - `/gps` on this
// one answers with something, and it is not a SA-MP checkpoint, because the
// checkpoint field reads empty while the radar plainly has a new mark on it.
// A server's mark is an ordinary coordinate blip in the same array as the
// player's own.
//
// So this reads the array itself rather than one handle out of it. What each
// mark means is not decided here: the position, the colour, the sprite and
// the raw bytes go out as they are found, and whoever asked works out which
// one is the destination. That is the same rule the rest of this module
// follows - it reports, it does not interpret.
//
#include <cstdint>
#include <string>
#include <vector>

#include "game/world_query.hpp"

namespace gtabot::game {

struct Blip {
  int           index = 0;
  Vec3          at;
  float         away_m = 0;
  std::uint32_t colour = 0;
  std::uint32_t entity = 0;      // non-zero when it follows a car or a person
  std::uint16_t counter = 0;
  std::uint8_t  flags = 0;
  std::uint8_t  kind = 0;        // the byte at 0x26, whatever it means
  bool          tracking = false;
  bool          fresh = false;      // not on the radar a moment ago
  std::string   bytes;           // the whole trace, for working out the rest
};

// The marks that have a position worth having. `from` is where the character
// is, for the distance.
std::vector<Blip> BlipsNow(const Vec3& from);

// Which trace the game itself calls the destination, or -1. The front-end
// menu manager keeps the handle of the map waypoint there; if a server's
// /gps sets that rather than merely drawing an icon, this names it and no
// guessing at bytes is needed.
int WaypointIndex();

// The mark that appeared most recently and is still there, or nothing.
//
// This is how a person notices where a server wants him: /gps answers, and a
// new mark is on the radar that was not there before. The game's own
// waypoint pointer stays empty for it, and picking it out of the bytes would
// be archaeology - so instead the marks are remembered between reads and the
// new one is named. The first read only remembers; nothing is new to
// somebody who has just opened his eyes.
bool AppearedLast(Blip* out);

}  // namespace gtabot::game
