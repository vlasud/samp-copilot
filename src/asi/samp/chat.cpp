#include "samp/chat.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "log.hpp"
#include "state/memory.hpp"

namespace gtabot::samp {
namespace {

// *(samp.dll + this) is the chat structure on 0.3.7-R1, according to the
// public headers. It is a starting guess and nothing more: it is accepted only
// if the block it points at actually holds a column of chat lines, and if it
// does not, the sweep below looks for one anywhere samp.dll keeps a pointer.
constexpr std::uint32_t kChatPointerRva = 0x21A0E4;

// How far into a candidate block to look. The ring is near the front of the
// structure that owns it, and scanning further costs more than it can find.
constexpr std::size_t kBlockScan = 0x40000;
// One entry has to hold a line of chat and the fields around it.
constexpr std::uint32_t kMinStride = 32;
constexpr std::uint32_t kMaxStride = 1024;
constexpr int           kMaxEntries = 256;
// A short run of coincidences is easy to come by; forty lines of text at a
// constant stride is not.
constexpr int kMinStrings = 12;
constexpr int kMinRun     = 16;
constexpr std::size_t kMaxFieldLength = 256;
// Field starts to use as a reference. Enough to walk past any header the
// structure has, bounded so the sweep cannot run away.
constexpr int kReferencesPrimary  = 512;
constexpr int kReferencesFallback = 128;
constexpr int kMaxRoots     = 512;
constexpr int kMaxSiblings  = 160;
// A block too small to hold a chat log is not one, and rejecting it up front
// is what keeps the fallback sweep affordable.
constexpr std::size_t kMinBlockSpan = 16u * 1024;
constexpr int         kMinBlockStrings = 24;
// A tick count in a chat entry is minutes or hours old, never days, and never
// in the future.
constexpr std::uint32_t kMaxAgeMs = 24u * 60 * 60 * 1000;
constexpr std::uint32_t kClockSlackMs = 5000;
constexpr int kMinTimeSamples = 8;

// Resolution walks a lot of memory, so a failure backs off rather than
// re-running on every frame the panel draws.
constexpr unsigned long long kRetryAfterMs = 5000;

ChatLayout        g_layout;
bool              g_resolved = false;
unsigned long long g_last_attempt_ms = 0;

// CP1251 is single-byte, so anything from 0x20 up is a character. What matters
// is the other direction: control bytes are what tell a string apart from a
// struct field, and there are none in chat.
inline bool Printable(unsigned char byte) { return byte >= 0x20; }

// A field start, not the middle of the field in front of it. Insisting the
// byte before is unprintable is what stops every suffix of a message from
// counting as another column.
inline bool FieldStart(const unsigned char* block, std::size_t size,
                       std::size_t at) {
  if (at + 3 > size) return false;
  if (at > 0 && Printable(block[at - 1])) return false;
  return Printable(block[at]) && Printable(block[at + 1]) &&
         Printable(block[at + 2]);
}

// A slot that is either a line or an unused entry. A ring that has not filled
// up yet is zeroes at the end, and refusing those would cut the run short.
inline bool FieldOrEmpty(const unsigned char* block, std::size_t size,
                         std::size_t at) {
  if (at >= size) return false;
  if (block[at] == 0) return true;
  return FieldStart(block, size, at);
}

struct Shape {
  std::uint32_t reference = 0;
  std::uint32_t stride    = 0;
  int           count     = 0;
  int           strings    = 0;
};

// Looks for a column of strings repeating at a constant stride.
//
// VirtualQuery only says a page was mapped a moment ago, so the whole walk is
// guarded: another thread is free to unmap it while we read, and a search must
// not be able to take the game down for that. No C++ objects here, which is
// what __try requires.
bool SearchRing(const unsigned char* block, std::size_t size,
                int max_references, Shape* best) {
  Shape winner;
  bool found = false;
  __try {
    const std::size_t limit = size < kBlockScan ? size : kBlockScan;
    int references = 0;
    for (std::size_t at = 0; at + 4 < limit && references < max_references;
         ++at) {
      if (!FieldStart(block, limit, at)) continue;
      ++references;

      for (std::uint32_t stride = kMinStride; stride <= kMaxStride;
           stride += 4) {
        int count   = 1;
        int strings = 1;
        for (int k = 1; k < kMaxEntries; ++k) {
          const std::size_t next =
              at + static_cast<std::size_t>(k) * stride;
          if (next + 4 > limit) break;
          if (!FieldOrEmpty(block, limit, next)) break;
          ++count;
          if (block[next] != 0) ++strings;
        }
        if (strings < kMinStrings || count < kMinRun) continue;

        // More lines wins. A tie goes to the tighter stride, because twice
        // the real stride reads every other entry and looks just as good.
        const bool better =
            strings > winner.strings ||
            (strings == winner.strings && count > winner.count) ||
            (strings == winner.strings && count == winner.count &&
             stride < winner.stride);
        if (!better) continue;
        winner.reference = static_cast<std::uint32_t>(at);
        winner.stride    = stride;
        winner.count     = count;
        winner.strings   = strings;
        found = true;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Whatever was established before the page went away still stands.
  }
  *best = winner;
  return found;
}

struct Sibling {
  std::int32_t delta        = 0;
  int          hits         = 0;
  int          total_length = 0;
};

// Every other column in the same entry. The message is one of these and so is
// the speaker; which is which is decided by the caller, from the numbers.
int ScanSiblings(const unsigned char* block, std::size_t size,
                 const Shape& shape, Sibling* out, int max_out) {
  int written = 0;
  __try {
    const std::int32_t span = static_cast<std::int32_t>(shape.stride);
    for (std::int32_t delta = -span + 1; delta < span && written < max_out;
         ++delta) {
      if (delta == 0) continue;
      int hits  = 0;
      int total = 0;
      for (int k = 0; k < shape.count; ++k) {
        const std::ptrdiff_t at =
            static_cast<std::ptrdiff_t>(shape.reference) +
            static_cast<std::ptrdiff_t>(k) * shape.stride + delta;
        if (at < 0 || static_cast<std::size_t>(at) + 4 > size) continue;
        if (!FieldStart(block, size, static_cast<std::size_t>(at))) continue;

        std::size_t length = 0;
        while (static_cast<std::size_t>(at) + length < size &&
               length < kMaxFieldLength && block[at + length] != 0) {
          if (!Printable(block[at + length])) { length = 0; break; }
          ++length;
        }
        if (length == 0) continue;
        ++hits;
        total += static_cast<int>(length);
      }
      // Present in a quarter of the lines or it is not a column.
      if (hits * 4 < shape.strings) continue;
      out[written].delta        = delta;
      out[written].hits         = hits;
      out[written].total_length = total;
      ++written;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return written;
}

// A column of tick counts: the same value never going backwards (or never
// forwards), all of them recent, and enough different values to rule out a
// constant. This is the only thing in the entry that says which end of the
// array is the newest line, so it is worth looking for.
bool ScanTimestamp(const unsigned char* block, std::size_t size,
                   const Shape& shape, std::uint32_t now_ticks,
                   std::int32_t* delta_out, bool* newest_first_out) {
  bool found = false;
  __try {
    const std::int32_t span = static_cast<std::int32_t>(shape.stride);
    for (std::int32_t delta = -span + 1; delta < span && !found; ++delta) {
      if ((shape.reference + delta) % 4 != 0) continue;

      std::uint32_t previous = 0;
      int samples   = 0;
      int distinct  = 0;
      bool rising   = true;
      bool falling  = true;
      bool sane     = true;

      for (int k = 0; k < shape.count && sane; ++k) {
        const std::ptrdiff_t base =
            static_cast<std::ptrdiff_t>(shape.reference) +
            static_cast<std::ptrdiff_t>(k) * shape.stride;
        // Only entries that hold a line: an unused one is all zeroes and
        // would look like a timestamp jumping back to nothing.
        if (base < 0 || static_cast<std::size_t>(base) + 4 > size) continue;
        if (block[base] == 0) continue;

        const std::ptrdiff_t at = base + delta;
        if (at < 0 || static_cast<std::size_t>(at) + 4 > size) { sane = false; break; }
        std::uint32_t value = 0;
        std::memcpy(&value, block + at, sizeof(value));

        if (value == 0) { sane = false; break; }
        const std::uint32_t age = now_ticks - value;
        if (age > kMaxAgeMs && now_ticks + kClockSlackMs < value) { sane = false; break; }
        if (samples > 0) {
          if (value < previous) rising  = false;
          if (value > previous) falling = false;
          if (value != previous) ++distinct;
        }
        previous = value;
        ++samples;
      }

      if (!sane) continue;
      if (samples < kMinTimeSamples || distinct < kMinTimeSamples / 2) continue;
      if (rising == falling) continue;  // constant, or neither
      *delta_out        = delta;
      *newest_first_out = falling;
      found = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return found;
}

// Copies one field out. Guarded for the same reason the searches are, and POD
// only so the guard is allowed.
int CopyField(const void* address, std::size_t max, char* out) {
  int length = -1;
  __try {
    const auto* bytes = static_cast<const unsigned char*>(address);
    std::size_t i = 0;
    while (i < max && bytes[i] != 0) {
      if (!Printable(bytes[i])) return -1;
      out[i] = static_cast<char>(bytes[i]);
      ++i;
    }
    out[i] = 0;
    length = static_cast<int>(i);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    length = -1;
  }
  return length;
}

std::string ReadField(std::uintptr_t address) {
  if (address == 0) return {};
  if (!asi::mem::IsReadable(address, 4)) return {};
  char buffer[kMaxFieldLength + 1];
  const int length = CopyField(reinterpret_cast<const void*>(address),
                               kMaxFieldLength, buffer);
  if (length <= 0) return {};
  return std::string(buffer, static_cast<std::size_t>(length));
}

bool IsHeapPointer(std::uintptr_t value) {
  if (value < 0x00010000u || value >= 0xC0000000u) return false;
  if (value % 4 != 0) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(value), &mbi, sizeof(mbi)))
    return false;
  return mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
         (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                         PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// How many bytes from `base` are committed and readable, up to `max`. The
// searches read raw for speed, so they need to know where to stop.
std::size_t ReadableSpan(std::uintptr_t base, std::size_t max) {
  std::size_t span = 0;
  std::uintptr_t cursor = base;
  while (span < max) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)))
      break;
    if (mbi.State != MEM_COMMIT) break;
    if ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_EXECUTE_WRITECOPY)) == 0)
      break;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) break;
    const std::uintptr_t region_end =
        reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (region_end <= cursor) break;
    span   = region_end - base;
    cursor = region_end;
  }
  return span < max ? span : max;
}

// A cheap first pass over a candidate block, so the fallback sweep does not
// run the full stride search on every pointer samp.dll happens to hold.
int CountFieldStarts(const unsigned char* block, std::size_t size,
                     int stop_at) {
  int count = 0;
  __try {
    const std::size_t limit = size < 0x10000u ? size : 0x10000u;
    for (std::size_t at = 0; at + 4 < limit && count < stop_at; ++at)
      if (FieldStart(block, limit, at)) ++count;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return count;
}

struct Candidate {
  std::uintptr_t block = 0;
  std::size_t    span  = 0;
  std::uint32_t  rva   = 0;
};

// Every pointer samp.dll holds that leads somewhere big enough and texty
// enough to be a chat log. Only reached when the documented offset does not
// pan out - which is the case this has to survive, because the whole point is
// to work on a build nobody wrote the offsets down for.
std::vector<Candidate> SweepRoots(const asi::mem::Module& samp) {
  std::vector<Candidate> found;
  const std::vector<asi::mem::Region> regions = asi::mem::ReadableRegions(&samp);

  for (const asi::mem::Region& region : regions) {
    if (!region.is_writable) continue;  // a root pointer lives in writable data
    const auto* words = reinterpret_cast<const std::uint32_t*>(region.base);
    const std::size_t count = region.size / sizeof(std::uint32_t);

    for (std::size_t i = 0; i < count; ++i) {
      if (found.size() >= static_cast<std::size_t>(kMaxRoots)) return found;
      const std::uintptr_t value = words[i];
      if (!IsHeapPointer(value)) continue;

      const std::size_t span = ReadableSpan(value, kBlockScan);
      if (span < kMinBlockSpan) continue;
      if (CountFieldStarts(reinterpret_cast<const unsigned char*>(value), span,
                           kMinBlockStrings) < kMinBlockStrings)
        continue;

      bool already = false;
      for (const Candidate& seen : found)
        if (seen.block == value) { already = true; break; }
      if (already) continue;

      Candidate candidate;
      candidate.block = value;
      candidate.span  = span;
      candidate.rva   = static_cast<std::uint32_t>(
          region.base + i * sizeof(std::uint32_t) - samp.base);
      found.push_back(candidate);
    }
  }
  return found;
}

// Turns a shape into a layout: which column is the message, which is the
// speaker, and which way round the array runs.
bool Describe(const Candidate& candidate, const Shape& shape,
              ChatLayout* layout) {
  const auto* block = reinterpret_cast<const unsigned char*>(candidate.block);

  Sibling siblings[kMaxSiblings];
  const int sibling_count =
      ScanSiblings(block, candidate.span, shape, siblings, kMaxSiblings);

  // The reference column is whichever field start came first in the block,
  // which inside an entry may well be the speaker rather than the message.
  // The message is the column that is present in more entries, and when both
  // are always present, the longer one.
  Sibling best;
  best.delta        = 0;
  best.hits         = shape.strings;
  best.total_length = 0;
  for (int k = 0; k < shape.count; ++k) {
    const std::size_t at =
        shape.reference + static_cast<std::size_t>(k) * shape.stride;
    if (at >= candidate.span || block[at] == 0) continue;
    std::size_t length = 0;
    while (at + length < candidate.span && length < kMaxFieldLength &&
           block[at + length] != 0)
      ++length;
    best.total_length += static_cast<int>(length);
  }

  const Sibling* rival = nullptr;
  for (int i = 0; i < sibling_count; ++i) {
    if (siblings[i].delta > -4 && siblings[i].delta < 4) continue;
    if (rival == nullptr || siblings[i].hits > rival->hits ||
        (siblings[i].hits == rival->hits &&
         siblings[i].total_length > rival->total_length))
      rival = &siblings[i];
  }

  std::int32_t text_delta   = 0;
  std::int32_t prefix_delta = 0;
  bool         has_prefix   = false;
  if (rival != nullptr) {
    has_prefix = true;
    const bool rival_is_message =
        rival->hits > best.hits ||
        (rival->hits == best.hits && rival->total_length > best.total_length);
    if (rival_is_message) {
      text_delta   = rival->delta;
      prefix_delta = -rival->delta;
    } else {
      text_delta   = 0;
      prefix_delta = rival->delta;
    }
  }

  layout->block      = candidate.block;
  layout->span       = candidate.span;
  layout->root_rva   = candidate.rva;
  layout->stride     = shape.stride;
  layout->entries    = shape.count;
  layout->populated  = shape.strings;
  layout->first_text = candidate.block + shape.reference + text_delta;
  layout->has_prefix = has_prefix;
  layout->prefix_delta = prefix_delta;

  std::int32_t time_delta = 0;
  bool newest_first = false;
  if (ScanTimestamp(block, candidate.span, shape, GetTickCount(), &time_delta,
                    &newest_first)) {
    layout->has_time     = true;
    // Recorded against the message column, so a reader never has to know
    // where the entry begins.
    layout->time_delta   = time_delta - text_delta;
    layout->newest_first = newest_first;
    layout->order_known  = true;
  }
  return true;
}

bool TryCandidate(const Candidate& candidate, int max_references,
                  ChatLayout* layout) {
  Shape shape;
  if (!SearchRing(reinterpret_cast<const unsigned char*>(candidate.block),
                  candidate.span, max_references, &shape))
    return false;
  return Describe(candidate, shape, layout);
}

}  // namespace

const ChatLayout& CachedChat() { return g_layout; }

void ForgetChat() {
  g_resolved = false;
  g_layout   = ChatLayout{};
}

const ChatLayout& ResolveChat() {
  if (g_resolved) return g_layout;
  const unsigned long long now = GetTickCount64();
  if (g_last_attempt_ms != 0 && now - g_last_attempt_ms < kRetryAfterMs)
    return g_layout;
  g_last_attempt_ms = now;

  ChatLayout layout;
  const asi::mem::Module samp = asi::mem::FindModule(L"samp.dll");
  if (!samp.valid()) {
    layout.note = "samp.dll is not loaded";
    g_layout    = layout;
    return g_layout;
  }

  // The documented pointer first. It costs one read to try, and when it is
  // right the sweep never has to run.
  std::uint32_t documented = 0;
  if (asi::mem::Read<std::uint32_t>(samp.base + kChatPointerRva, &documented) &&
      IsHeapPointer(documented)) {
    Candidate candidate;
    candidate.block = documented;
    candidate.span  = ReadableSpan(documented, kBlockScan);
    candidate.rva   = kChatPointerRva;
    layout.roots_tried = 1;
    if (candidate.span >= kMinBlockSpan &&
        TryCandidate(candidate, kReferencesPrimary, &layout)) {
      layout.valid = true;
      layout.note  = "chat found through the documented pointer";
    }
  }

  if (!layout.valid) {
    const std::vector<Candidate> roots = SweepRoots(samp);
    layout.roots_tried += static_cast<int>(roots.size());
    for (const Candidate& candidate : roots) {
      ChatLayout attempt;
      if (!TryCandidate(candidate, kReferencesFallback, &attempt)) continue;
      // The best of the sweep, not the first: several blocks hold text at a
      // regular stride, and the chat log is the deepest of them.
      if (attempt.populated <= layout.populated) continue;
      attempt.valid = true;
      attempt.note  = "chat found by sweeping samp.dll's pointers - the "
                      "documented offset did not lead to it";
      const int tried = layout.roots_tried;
      layout = attempt;
      layout.roots_tried = tried;
    }
  }

  if (!layout.valid) {
    layout.note = "no column of chat lines found in " +
                  std::to_string(layout.roots_tried) +
                  " candidate blocks - connect to a server and let some chat "
                  "arrive first";
    g_layout = layout;
    return g_layout;  // keep trying; the chat fills up after joining
  }

  g_layout   = layout;
  g_resolved = true;
  LOG_INFO("chat resolved: block=0x{:08X} via samp.dll+0x{:X} stride={} "
           "entries={} populated={} prefix={} order={}",
           layout.block, layout.root_rva, layout.stride, layout.entries,
           layout.populated,
           layout.has_prefix ? std::to_string(layout.prefix_delta)
                             : std::string("none"),
           layout.order_known
               ? std::string(layout.newest_first ? "newest first"
                                                 : "oldest first")
               : std::string("unknown"));
  return g_layout;
}

json ReadChat(int limit) {
  const ChatLayout& layout = ResolveChat();

  json out;
  out["valid"] = layout.valid;
  out["note"]  = layout.note;
  if (!layout.valid) {
    out["lines"] = json::array();
    return out;
  }

  out["stride"]  = layout.stride;
  out["entries"] = layout.entries;
  out["source"]  = "samp.dll+0x" + [&] {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%X", layout.root_rva);
    return std::string(buffer);
  }();
  out["order"] = layout.order_known
                     ? (layout.newest_first ? "newest first" : "oldest first")
                     : "unknown";

  const std::uint32_t now_ticks = GetTickCount();
  std::vector<json> lines;
  lines.reserve(static_cast<std::size_t>(layout.entries));
  for (int i = 0; i < layout.entries; ++i) {
    const std::uintptr_t at =
        layout.first_text + static_cast<std::uintptr_t>(i) * layout.stride;
    const std::string text = ReadField(at);
    if (text.empty()) continue;

    json line;
    line["text"] = ToUtf8(text);
    if (layout.has_prefix) {
      const std::string from = ReadField(at + layout.prefix_delta);
      if (!from.empty()) line["from"] = ToUtf8(from);
    }
    if (layout.has_time) {
      std::uint32_t stamp = 0;
      if (asi::mem::Read<std::uint32_t>(at + layout.time_delta, &stamp) &&
          stamp != 0)
        line["age_ms"] = static_cast<std::int64_t>(now_ticks - stamp);
    }
    lines.push_back(std::move(line));
  }

  // Oldest first, whichever way the array runs.
  if (layout.newest_first) std::reverse(lines.begin(), lines.end());
  out["count"] = lines.size();
  if (limit > 0 && lines.size() > static_cast<std::size_t>(limit))
    lines.erase(lines.begin(),
                lines.end() - static_cast<std::ptrdiff_t>(limit));
  out["lines"] = std::move(lines);
  return out;
}

bool DumpChat() {
  const ChatLayout& layout = ResolveChat();
  const std::string path = ModuleDirectory() + "bot.chat-dump.txt";
  std::ofstream file(path, std::ios::trunc);
  if (!file) return false;

  file << "gtabot chat dump\n================\n\n";
  if (!layout.valid) {
    file << "not resolved: " << layout.note << "\n";
    LOG_WARN("chat dump: {}", layout.note);
    return true;
  }

  char header[256];
  std::snprintf(header, sizeof(header),
                "block      0x%08X  (%u bytes readable)\n"
                "pointer    samp.dll+0x%X\n"
                "stride     %u bytes\n"
                "entries    %d, %d of them holding a line\n"
                "message    at the reference column\n"
                "speaker    %s\n"
                "timestamp  %s\n"
                "order      %s\n",
                static_cast<unsigned>(layout.block),
                static_cast<unsigned>(layout.span), layout.root_rva,
                layout.stride, layout.entries, layout.populated,
                layout.has_prefix
                    ? (std::to_string(layout.prefix_delta) +
                       " bytes from it").c_str()
                    : "none found",
                layout.has_time
                    ? (std::to_string(layout.time_delta) +
                       " bytes from it").c_str()
                    : "none found",
                layout.order_known
                    ? (layout.newest_first ? "newest first" : "oldest first")
                    : "unknown - no timestamp column to settle it");
  file << header << "\n";

  // The raw bytes of the last few entries, so a column this pass got wrong
  // can be read off by hand rather than guessed at again.
  constexpr int kDumpEntries = 8;
  const int from = layout.entries > kDumpEntries ? layout.entries - kDumpEntries
                                                 : 0;
  for (int i = from; i < layout.entries; ++i) {
    const std::uintptr_t at =
        layout.first_text + static_cast<std::uintptr_t>(i) * layout.stride;
    char title[96];
    std::snprintf(title, sizeof(title), "\n--- entry %d at 0x%08X\n", i,
                  static_cast<unsigned>(at));
    file << title;

    const std::int32_t begin = layout.has_prefix && layout.prefix_delta < 0
                                   ? layout.prefix_delta
                                   : -16;
    for (std::int32_t offset = begin;
         offset < static_cast<std::int32_t>(layout.stride); offset += 16) {
      char line[160];
      char ascii[17] = {};
      unsigned bytes[16] = {};
      for (int b = 0; b < 16; ++b) {
        unsigned char value = 0;
        if (!asi::mem::Read<unsigned char>(at + offset + b, &value)) value = 0;
        bytes[b] = value;
        ascii[b] = (value >= 0x20 && value < 0x7F) ? static_cast<char>(value)
                                                   : '.';
      }
      std::snprintf(line, sizeof(line),
                    "  %+5d  %02X %02X %02X %02X %02X %02X %02X %02X "
                    "%02X %02X %02X %02X %02X %02X %02X %02X  |%s|\n",
                    static_cast<int>(offset), bytes[0], bytes[1], bytes[2],
                    bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
                    bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
                    bytes[14], bytes[15], ascii);
      file << line;
    }
  }

  file << "\n";
  LOG_INFO("wrote {}", path);
  return true;
}

}  // namespace gtabot::samp
