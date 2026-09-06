#pragma once
//
// Works out where SA-MP keeps its data, from evidence rather than from a table
// of offsets copied off a forum.
//
// The starting point is the one fact we did not have to find in memory: the
// nickname the launcher passed on the command line. Wherever that string turns
// up in the heap, it is sitting inside a live SA-MP structure - so the bytes
// around it, and whatever points at them, describe the layout of the player
// pool for this exact build.
//
// The result is a written report rather than a parsed structure. Guessing a
// layout and reading it back is how a mod ends up quietly reporting nonsense;
// the offsets get committed to code only once the report shows what they are.
//
#include <string>

#include "types.hpp"

namespace gtabot::samp {

struct ReportOutcome {
  bool        written = false;
  // Occurrences found outside samp.dll's own image, i.e. in live structures.
  std::size_t heap_hits = 0;
  std::size_t module_hits = 0;
  std::string path;
  std::string error;
};

// Game thread only, and it is not cheap: a sweep of committed private memory
// for the needle, then a second sweep for pointers into what it found. Costs a
// visible hitch, which is why it is asked for rather than run every frame.
//
// `needle` empty means "the nickname from the command line".
ReportOutcome WriteStructureReport(const std::string& needle = {});

// True once a report has been written, so the automatic attempt stops.
bool report_written();

}  // namespace gtabot::samp
