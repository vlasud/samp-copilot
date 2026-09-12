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
  // Occurrences found outside samp.dll's own image.
  std::size_t heap_hits = 0;
  std::size_t module_hits = 0;
  // Of those, the ones that are just another copy of the process command line.
  // The launcher passes the nickname there, so the process is littered with
  // them and none of them is a SA-MP structure.
  std::size_t command_line_hits = 0;
  // What is left: occurrences worth looking at.
  std::size_t structure_hits = 0;
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

// Game thread only. Reads a run of words by address and says what each one
// plausibly is, with the same classification the report uses. The hand lens
// for a layout the shape search could not settle by itself: cheap, explicit,
// and nothing in the module reads the world through it.
//
// `address` is hex ("0x048B63A0") or decimal; `stride` is the bytes between
// the words read, so an array of records can be stepped along instead of read
// whole; `as_text` also reads each word's own bytes as text, which is how an
// array of names gives itself away.
json ReadWords(const std::string& address, int words, int stride, bool as_text);

}  // namespace gtabot::samp
