#include "samp/discovery.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "log.hpp"
#include "samp/version.hpp"
#include "state/memory.hpp"

namespace gtabot::samp {
namespace {

// How much of the structure around a hit to describe. SA-MP's player pool is
// large, but what identifies it - the local id, the name, the pointer arrays -
// sits close by.
constexpr std::ptrdiff_t kContextBefore = 0x80;
constexpr std::ptrdiff_t kContextAfter  = 0x140;
// How far back from a hit a pointer may land and still plausibly be a pointer
// to the structure containing it.
constexpr std::uintptr_t kPointerWindow = 0x1000;
// Caps, so a sweep of a multi-gigabyte process cannot stall the frame.
constexpr std::size_t kScanBudget   = 768u * 1024 * 1024;
constexpr std::size_t kMaxNeedleHits = 24;
constexpr std::size_t kMaxReferences = 24;
// The reference sweep runs per hit and is the expensive half, so only the
// first few live copies get the full treatment.
constexpr std::size_t kMaxDetailed = 6;

bool g_written = false;

std::string Hex(std::uintptr_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(value));
  return buffer;
}

// Says what a 4-byte value most plausibly is. This is the whole point of the
// report: a column of raw hex tells you nothing, but "-> samp.dll+0x219A6F"
// next to "int 42" next to a readable name is a structure you can read.
std::string Classify(std::uint32_t value, const asi::mem::Module& samp,
                     const asi::mem::Module& game) {
  if (value == 0) return "0";
  if (samp.valid() && samp.contains(value))
    return "-> samp.dll+" + Hex(value - samp.base);
  if (game.valid() && game.contains(value))
    return "-> gta_sa.exe+" + Hex(value - game.base);

  if (value >= 0x00100000u && value < 0xC0000000u && value % 4 == 0) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(value), &mbi, sizeof(mbi)) &&
        mbi.State == MEM_COMMIT) {
      const std::string kind = mbi.Type == MEM_PRIVATE ? "heap" : "mapped";
      // A pointer worth following is one whose target reads as a structure.
      std::string preview;
      std::uint32_t first = 0;
      if (asi::mem::Read<std::uint32_t>(value, &first)) preview = " [" + Hex(first) + "]";
      return "-> " + kind + " " + Hex(value) + preview;
    }
  }

  if (value < 4096) return "int " + std::to_string(value);

  const float as_float = *reinterpret_cast<const float*>(&value);
  if (as_float > -100000.0f && as_float < 100000.0f &&
      (as_float > 0.0001f || as_float < -0.0001f)) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "float %.3f", as_float);
    return buffer;
  }
  return "";
}

// The launcher puts the nickname on the command line, and Windows and the CRT
// leave copies of that string all over the process. Every one of them will
// match the needle and none of them is a SA-MP structure, so they are worth
// naming rather than analysing.
bool LooksLikeCommandLine(std::uintptr_t hit) {
  constexpr std::ptrdiff_t kWindow = 0x100;
  const char* marker = "gta_sa.exe";
  const std::size_t marker_length = 10;

  for (std::ptrdiff_t offset = -kWindow; offset < kWindow; ++offset) {
    bool matched = true;
    for (std::size_t i = 0; i < marker_length && matched; ++i) {
      char byte = 0;
      if (!asi::mem::Read<char>(hit + offset + i, &byte)) return false;
      // The copies alternate between narrow and wide, so compare loosely.
      matched = byte == marker[i];
    }
    if (matched) return true;
  }
  return false;
}

std::string AsciiOf(std::uintptr_t address, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) {
    unsigned char byte = 0;
    if (!asi::mem::Read<unsigned char>(address + i, &byte)) return out;
    out += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
  }
  return out;
}

void DescribeNeighbourhood(std::ostream& out, std::uintptr_t hit,
                           const asi::mem::Module& samp, const asi::mem::Module& game) {
  for (std::ptrdiff_t offset = -kContextBefore; offset < kContextAfter;
       offset += 4) {
    const std::uintptr_t address = hit + offset;
    std::uint32_t value = 0;
    if (!asi::mem::Read<std::uint32_t>(address, &value)) continue;

    char line[160];
    std::snprintf(line, sizeof(line), "    %+6d  %s  %-8s  %-34s |%s|",
                  static_cast<int>(offset), Hex(address).c_str(),
                  Hex(value).substr(2).c_str(),
                  Classify(value, samp, game).c_str(),
                  AsciiOf(address, 4).c_str());
    out << line << "\n";
  }
}

// Everything that points into [hit - window, hit + 16]. Whatever holds the
// structure containing the name shows up here, and that is the thread back to
// SA-MP's root pointer.
void DescribeReferences(std::ostream& out, std::uintptr_t hit,
                        const std::vector<asi::mem::Region>& regions,
                        const asi::mem::Module& samp, const asi::mem::Module& game) {
  const std::uintptr_t low  = hit > kPointerWindow ? hit - kPointerWindow : 0;
  const std::uintptr_t high = hit + 16;

  std::size_t found = 0;
  std::size_t scanned = 0;
  for (const asi::mem::Region& region : regions) {
    if (found >= kMaxReferences || scanned >= kScanBudget) break;
    if (!region.is_writable) continue;
    scanned += region.size;

    const auto* values = reinterpret_cast<const std::uint32_t*>(region.base);
    const std::size_t count = region.size / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < count && found < kMaxReferences; ++i) {
      const std::uintptr_t value = values[i];
      if (value < low || value > high) continue;

      const std::uintptr_t at = region.base + i * sizeof(std::uint32_t);
      std::string where = Hex(at);
      if (samp.valid() && samp.contains(at))
        where = "samp.dll+" + Hex(at - samp.base);
      else if (game.valid() && game.contains(at))
        where = "gta_sa.exe+" + Hex(at - game.base);

      char line[160];
      std::snprintf(line, sizeof(line),
                    "    %-24s holds %s  (name is at +%d from it)",
                    where.c_str(), Hex(value).c_str(),
                    static_cast<int>(hit - value));
      out << line << "\n";
      ++found;
    }
  }
  if (found == 0) out << "    (nothing points here)\n";
}

}  // namespace

bool report_written() { return g_written; }

ReportOutcome WriteStructureReport(const std::string& requested_needle) {
  ReportOutcome outcome;

  const Client client = Detect();
  if (!client.base) {
    outcome.error = "samp.dll is not loaded";
    return outcome;
  }

  std::string needle = requested_needle;
  if (needle.empty()) {
    // Same ground truth the probe uses: whatever the launcher put after -n.
    const std::string command_line = GetCommandLineA();
    const std::size_t at = command_line.find("-n ");
    if (at != std::string::npos) {
      std::size_t begin = at + 3;
      while (begin < command_line.size() && command_line[begin] == ' ') ++begin;
      std::size_t end = command_line.find(' ', begin);
      if (end == std::string::npos) end = command_line.size();
      needle = command_line.substr(begin, end - begin);
    }
  }
  if (needle.empty()) {
    outcome.error = "no nickname on the command line and none given";
    return outcome;
  }

  const asi::mem::Module samp = asi::mem::FindModule(L"samp.dll");
  const asi::mem::Module game = asi::mem::FindModule(nullptr);

  const std::vector<asi::mem::Region> everything = asi::mem::ReadableRegions();
  const std::vector<asi::mem::Hit> hits =
      asi::mem::Scan(everything, needle, kMaxNeedleHits, kScanBudget);

  std::vector<bool> is_command_line(hits.size(), false);
  for (std::size_t i = 0; i < hits.size(); ++i) {
    if (samp.contains(hits[i].address)) {
      ++outcome.module_hits;
      continue;
    }
    ++outcome.heap_hits;
    is_command_line[i] = LooksLikeCommandLine(hits[i].address);
    if (is_command_line[i])
      ++outcome.command_line_hits;
    else
      ++outcome.structure_hits;
  }

  if (outcome.heap_hits == 0) {
    outcome.error =
        "'" + needle +
        "' is only in samp.dll's own image, not in any live structure - "
        "connect to a server and get in-game before asking";
    return outcome;
  }
  if (outcome.structure_hits == 0) {
    outcome.error =
        "every live copy of '" + needle +
        "' is just another copy of the process command line, so none of them "
        "is a SA-MP structure. On a roleplay server the name above your "
        "character is not the launcher nickname - type that name into the "
        "panel and dump again";
    return outcome;
  }

  outcome.path = ModuleDirectory() + "bot.samp-report.txt";
  std::ofstream file(outcome.path, std::ios::trunc);
  if (!file) {
    outcome.error = "could not write " + outcome.path;
    return outcome;
  }

  file << "gtabot SA-MP structure report\n"
       << "=============================\n\n"
       << "samp.dll   base " << Hex(samp.base) << "  size " << Hex(samp.size)
       << "  version " << ToString(client.version) << "\n"
       << "gta_sa.exe base " << Hex(game.base) << "\n"
       << "nickname   \"" << needle << "\" (" << needle.size() << " chars)\n"
       << "found      " << outcome.module_hits << " in the image, "
       << outcome.heap_hits << " elsewhere\n\n"
       << "Columns are: offset from the hit, address, raw value, what the value\n"
       << "most plausibly is, and the four bytes as text.\n";

  int index = 0;
  for (const asi::mem::Hit& hit : hits) {
    const bool in_module = samp.contains(hit.address);
    file << "\n\n--- [" << ++index << "] " << Hex(hit.address);
    if (in_module) {
      file << "  samp.dll+" << Hex(hit.address - samp.base)
           << "  (inside the image - the startup copy, not a live structure)\n";
      continue;
    }
    file << "  outside any module - a live structure\n\n"
         << "  who points at it:\n";
    DescribeReferences(file, hit.address, everything, samp, game);
    file << "\n  around it:\n";
    DescribeNeighbourhood(file, hit.address, samp, game);
  }

  file << "\n";
  outcome.written = true;
  g_written = true;
  LOG_INFO("wrote {} ({} live occurrences of '{}')", outcome.path,
           outcome.heap_hits, needle);
  return outcome;
}

}  // namespace gtabot::samp
