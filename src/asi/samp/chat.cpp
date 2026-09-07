#include "samp/chat.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "log.hpp"
#include "samp/world.hpp"
#include "state/memory.hpp"

namespace gtabot::samp {
namespace {

// *(samp.dll + this) is the chat structure on 0.3.7-R1, according to the
// public headers. It is a starting guess and nothing more: it is accepted only
// if the block behind it holds something that reads like a chat log.
constexpr std::uint32_t kChatPointerRva = 0x21A0E4;

// How far into a candidate block to look. The ring is near the front of the
// structure that owns it, and scanning further costs more than it can find.
constexpr std::size_t kBlockScan = 0x40000;

// An entry has to hold a line of chat, and SA-MP's is 144 characters. A
// 48-byte record cannot be one however regular it looks, and the first version
// of this search proved the point by picking exactly that out of a table of
// short tokens and reporting it as the chat.
constexpr std::uint32_t kMinStride = 128;
constexpr std::uint32_t kMaxStride = 1024;
constexpr int           kMaxEntries = 256;

// What tells a chat log from any other array of strings: the entries are
// sentences. Twelve characters with a space in them is a low bar for a line of
// chat and an impossible one for a table of names, tokens or file extensions.
constexpr std::size_t kSentenceLength = 12;
constexpr int         kMinSentences   = 6;
constexpr int         kMinStrings     = 6;
constexpr int         kMinRun         = 8;

constexpr std::size_t kMaxFieldLength = 256;
// Field starts to use as a reference. Enough to walk past any header the
// structure has, bounded so the sweep cannot run away.
constexpr int kReferencesPrimary  = 512;
constexpr int kReferencesFallback = 128;
constexpr int kMaxRoots     = 256;
constexpr int kMaxColumns   = 192;
// How much of a region the sweep copies out at a time before looking at it.
constexpr std::size_t kSweepWords = 2048;  // 8 KB
// Candidates that get the full stride search. The cheap filter below decides
// which ones, and this caps what the expensive half can ever cost.
constexpr int kMaxSearches  = 48;
// A block too small to hold a chat log is not one, and rejecting it up front
// is what keeps the fallback sweep affordable.
constexpr std::size_t kMinBlockSpan = 16u * 1024;
constexpr int         kMinBlockStrings = 16;
constexpr std::size_t kFilterScan   = 8u * 1024;
// How far into a swept candidate to look. The documented pointer gets the full
// window because there is only one of it.
constexpr std::size_t kSweepScan    = 32u * 1024;

// The whole of resolution runs inside a frame, so it gets a deadline. Without
// one a search that turns out to be expensive on some build does not slow the
// game down, it stops it - which is exactly what an unbounded sweep of
// samp.dll's pointers did here.
constexpr unsigned long long kResolveBudgetMs = 40;
// The anchored search sweeps the heap, which no budget can make quick. It is
// allowed a visible stutter because it only runs a few times in a session and
// only when the cheap search has already failed.
constexpr unsigned long long kAnchorBudgetMs = 2000;

// A tick count in a chat entry is minutes or hours old, never days, and never
// in the future.
constexpr std::uint32_t kMaxAgeMs = 24u * 60 * 60 * 1000;
constexpr std::uint32_t kClockSlackMs = 5000;
// Four is enough to establish a direction, and there are only a handful of
// lines in the chat in the seconds after joining - which is exactly when this
// runs.
constexpr int kMinTimeSamples  = 4;
constexpr int kMinTimeDistinct = 2;
constexpr std::size_t kMaxNameLength = 32;

// The two clocks a client might stamp a line with: milliseconds since the
// machine booted, or seconds since 1970. Which one it is gets decided by
// which one the numbers fall on, not by which is more usual.
constexpr int kClockTicks   = 0;
constexpr int kClockSeconds = 1;

// The anchored search sweeps the whole process, which costs the game a visible
// stutter, so it runs a few times and then gives up rather than every retry
// for the rest of the session.
constexpr std::size_t kScanBudget    = 768u * 1024 * 1024;
constexpr std::size_t kMaxAnchorHits = 64;
constexpr int         kMaxAnchoredAttempts = 3;
// Confirming a stride means reading every entry of the ring it implies, so
// the number of strides allowed to get that far is capped too.
constexpr int         kMaxConfirmations = 8;

// Resolution walks a lot of memory, so a failure backs off rather than
// re-running on every frame.
constexpr unsigned long long kRetryAfterMs = 5000;

ChatLayout         g_layout;
bool               g_resolved = false;
unsigned long long g_last_attempt_ms = 0;
int                g_anchored_attempts = 0;

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

// Anything in this window that reads like a line someone wrote. Deliberately
// blind to where the columns are: the anchor offset lands at whichever field
// happened to come first, and the sentence may well be in a different one.
inline bool SentenceInWindow(const unsigned char* block, std::size_t size,
                             std::size_t begin, std::uint32_t stride) {
  std::size_t end = begin + stride;
  if (end > size) end = size;
  std::size_t run = 0;
  bool space = false;
  for (std::size_t i = begin; i < end; ++i) {
    if (!Printable(block[i])) {
      run = 0;
      space = false;
      continue;
    }
    ++run;
    if (block[i] == 0x20) space = true;
    if (run >= kSentenceLength && space) return true;
  }
  return false;
}

struct Shape {
  std::uint32_t reference = 0;
  std::uint32_t stride    = 0;
  int           count     = 0;
  int           strings   = 0;
  int           sentences = 0;
};

// How chat-like a candidate record layout is. Only ever called for a shape
// that already survived the run test, so the cost of walking every entry in
// full is paid rarely.
int CountSentences(const unsigned char* block, std::size_t size,
                   std::size_t at, std::uint32_t stride, int count) {
  int sentences = 0;
  __try {
    for (int k = 0; k < count; ++k) {
      const std::size_t begin = at + static_cast<std::size_t>(k) * stride;
      if (begin >= size) break;
      if (SentenceInWindow(block, size, begin, stride)) ++sentences;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return sentences;
}

// Looks for a record layout whose entries read like chat.
//
// VirtualQuery only says a page was mapped a moment ago, so the whole walk is
// guarded: another thread is free to unmap it while we read, and a search must
// not be able to take the game down for that. No C++ objects here, which is
// what __try requires.
bool SearchRing(const unsigned char* block, std::size_t size,
                std::size_t scan, int max_references, Shape* best) {
  Shape winner;
  bool found = false;
  __try {
    const std::size_t limit = size < scan ? size : scan;
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
          const std::size_t next = at + static_cast<std::size_t>(k) * stride;
          if (next + 4 > limit) break;
          if (!FieldOrEmpty(block, limit, next)) break;
          ++count;
          if (block[next] != 0) ++strings;
        }
        if (strings < kMinStrings || count < kMinRun) continue;

        const int sentences = CountSentences(block, limit, at, stride, count);
        if (sentences < kMinSentences) continue;

        // Lines of chat win, not records: the first version of this scored on
        // the number of strings and a table of four-character tokens beat the
        // chat by sheer length. A tie goes to the tighter stride, because
        // twice the real stride reads every other entry and looks as good.
        const bool better =
            sentences > winner.sentences ||
            (sentences == winner.sentences && stride < winner.stride) ||
            (sentences == winner.sentences && stride == winner.stride &&
             strings > winner.strings);
        if (!better) continue;
        winner.reference = static_cast<std::uint32_t>(at);
        winner.stride    = stride;
        winner.count     = count;
        winner.strings   = strings;
        winner.sentences = sentences;
        found = true;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Whatever was established before the page went away still stands.
  }
  *best = winner;
  return found;
}

// The extent of a ring around an entry already known to be one. Used by the
// anchored search, which starts from a line we can prove is chat rather than
// from the first field start in a block.
//
// Backwards only over entries that hold text: an unused slot is zeroes, and
// zeroes are also what lies before the array, so allowing them would walk out
// of the structure entirely. Forwards they are allowed, because a ring that
// has not filled up yet ends in them.
void ExpandRun(const unsigned char* block, std::size_t size, std::size_t at,
               std::uint32_t stride, std::size_t* first, int* count) {
  std::size_t begin = at;
  int total = 1;
  __try {
    while (begin >= stride && total < kMaxEntries) {
      const std::size_t previous = begin - stride;
      if (!FieldStart(block, size, previous)) break;
      begin = previous;
      ++total;
    }
    for (int k = 1; total < kMaxEntries; ++k) {
      const std::size_t next = at + static_cast<std::size_t>(k) * stride;
      if (next + 4 > size) break;
      if (!FieldOrEmpty(block, size, next)) break;
      ++total;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  *first = begin;
  *count = total;
}

int CountStrings(const unsigned char* block, std::size_t size, std::size_t at,
                 std::uint32_t stride, int count) {
  int strings = 0;
  __try {
    for (int k = 0; k < count; ++k) {
      const std::size_t next = at + static_cast<std::size_t>(k) * stride;
      if (next >= size) break;
      if (block[next] != 0) ++strings;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return strings;
}

// SA-MP will not let a player be called anything else: the protocol limits a
// nickname to these characters, so a column claiming to hold one and holding
// a byte outside them is holding something else. A colour, for instance -
// which is what the first version of this proudly reported as the speaker of
// every server message.
inline bool NameByte(unsigned char byte) {
  return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
         (byte >= '0' && byte <= '9') || byte == '_' || byte == '[' ||
         byte == ']' || byte == '(' || byte == ')' || byte == '$' ||
         byte == '@' || byte == '.' || byte == '=' || byte == ':' ||
         byte == '-' || byte == ' ';
}

// Seconds since 1970, the other clock a line might be stamped with.
std::uint32_t NowSeconds() {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER packed;
  packed.LowPart  = now.dwLowDateTime;
  packed.HighPart = now.dwHighDateTime;
  // FILETIME counts hundreds of nanoseconds from 1601.
  constexpr std::uint64_t kToUnix = 116444736000000000ull;
  if (packed.QuadPart < kToUnix) return 0;
  return static_cast<std::uint32_t>((packed.QuadPart - kToUnix) / 10000000ull);
}

inline std::int32_t Magnitude(std::int32_t value) {
  return value < 0 ? -value : value;
}

struct Column {
  std::int32_t delta        = 0;
  int          hits         = 0;
  int          sentences    = 0;
  int          total_length = 0;
  // Values that could be somebody's name rather than four bytes of something
  // else that happen to be printable.
  int          namelike     = 0;
};

// Every column in the entry, the reference one included. Which of them is the
// message and which is the speaker is decided by the caller, from these
// numbers rather than from a declaration.
int ScanColumns(const unsigned char* block, std::size_t size,
                const Shape& shape, Column* out, int max_out) {
  int written = 0;
  __try {
    const std::int32_t span = static_cast<std::int32_t>(shape.stride);
    for (std::int32_t delta = -span + 1; delta < span && written < max_out;
         ++delta) {
      int hits      = 0;
      int total     = 0;
      int sentences = 0;
      int namelike  = 0;
      for (int k = 0; k < shape.count; ++k) {
        const std::ptrdiff_t at =
            static_cast<std::ptrdiff_t>(shape.reference) +
            static_cast<std::ptrdiff_t>(k) * shape.stride + delta;
        if (at < 0 || static_cast<std::size_t>(at) + 4 > size) continue;
        if (!FieldStart(block, size, static_cast<std::size_t>(at))) continue;

        std::size_t length = 0;
        bool space = false;
        bool name  = true;
        while (static_cast<std::size_t>(at) + length < size &&
               length < kMaxFieldLength && block[at + length] != 0) {
          if (!Printable(block[at + length])) { length = 0; break; }
          if (block[at + length] == 0x20) space = true;
          if (!NameByte(block[at + length])) name = false;
          ++length;
        }
        if (length == 0) continue;
        ++hits;
        total += static_cast<int>(length);
        if (length >= kSentenceLength && space) ++sentences;
        if (name && length >= 2 && length <= kMaxNameLength) ++namelike;
      }
      // Present in a quarter of the lines or it is not a column.
      if (hits * 4 < shape.strings) continue;
      out[written].delta        = delta;
      out[written].hits         = hits;
      out[written].sentences    = sentences;
      out[written].total_length = total;
      out[written].namelike     = namelike;
      ++written;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return written;
}

// Whether a number is a clock reading taken in the last day, on one of the
// two clocks a client might have used to stamp a line.
inline bool OnClock(std::uint32_t value, int base, std::uint32_t now_ticks,
                    std::uint32_t now_seconds) {
  if (value == 0) return false;
  const std::uint32_t now = base == kClockTicks ? now_ticks : now_seconds;
  const std::uint32_t window =
      base == kClockTicks ? kMaxAgeMs : kMaxAgeMs / 1000;
  const std::uint32_t gap = value > now ? value - now : now - value;
  return gap <= window;
}

// The column that says when each line arrived.
//
// Worth more than the timestamp: it is what settles which end of the array is
// the newest line, and it is the only thing that tells a live entry from a
// slot the client has not written yet. The buffer is not zeroed, so an unused
// slot holds whatever the allocator left there - which is how a five-letter
// scrap of somebody else's memory ended up printed as the last line of chat.
//
// So readings outside the window are tolerated rather than fatal. They are
// precisely the entries we want to find, and demanding that every entry be
// recent - as the first version did - meant one stale slot rejected the whole
// column and left us with no way to spot it.
bool ScanClock(const unsigned char* block, std::size_t size, const Shape& shape,
               std::uint32_t now_ticks, std::uint32_t now_seconds,
               std::int32_t* delta_out, int* base_out, bool* newest_first_out) {
  bool found = false;
  __try {
    const std::int32_t span = static_cast<std::int32_t>(shape.stride);
    for (std::int32_t delta = -span + 1; delta < span && !found; ++delta) {
      if ((shape.reference + delta) % 4 != 0) continue;

      for (int base = kClockTicks; base <= kClockSeconds && !found; ++base) {
        std::uint32_t previous = 0;
        int live     = 0;   // entries holding a line
        int on_clock = 0;   // of those, ones with a plausible reading
        int distinct = 0;
        bool rising  = true;
        bool falling = true;
        bool readable = true;

        for (int k = 0; k < shape.count && readable; ++k) {
          const std::ptrdiff_t entry =
              static_cast<std::ptrdiff_t>(shape.reference) +
              static_cast<std::ptrdiff_t>(k) * shape.stride;
          if (entry < 0 || static_cast<std::size_t>(entry) + 4 > size) continue;
          if (block[entry] == 0) continue;
          ++live;

          const std::ptrdiff_t at = entry + delta;
          if (at < 0 || static_cast<std::size_t>(at) + 4 > size) {
            readable = false;
            break;
          }
          std::uint32_t value = 0;
          std::memcpy(&value, block + at, sizeof(value));
          if (!OnClock(value, base, now_ticks, now_seconds)) continue;

          if (on_clock > 0) {
            if (value < previous) rising = false;
            if (value > previous) falling = false;
            if (value != previous) ++distinct;
          }
          previous = value;
          ++on_clock;
        }

        if (!readable) continue;
        if (on_clock < kMinTimeSamples) continue;
        // Most of the lines, not a lucky handful: a column of four arbitrary
        // bytes will occasionally look like a clock in a couple of entries.
        if (on_clock * 2 < live) continue;
        if (distinct < kMinTimeDistinct) continue;
        if (rising == falling) continue;  // constant, or neither
        *delta_out        = delta;
        *base_out         = base;
        *newest_first_out = falling;
        found = true;
      }
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
                     std::size_t scan, int stop_at) {
  int count = 0;
  __try {
    const std::size_t limit = size < scan ? size : scan;
    for (std::size_t at = 0; at + 4 < limit && count < stop_at; ++at)
      if (FieldStart(block, limit, at)) ++count;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return count;
}

// Walks back from a byte inside a string to where the string began. Guarded
// like every other raw walk here.
std::size_t FieldStartBefore(const unsigned char* block, std::size_t at) {
  __try {
    while (at > 0 && Printable(block[at - 1])) --at;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return at;
}

struct Candidate {
  std::uintptr_t block = 0;
  std::size_t    span  = 0;
  std::uint32_t  rva   = 0;
};

// Which committed region an address falls in, by binary search over a list
// taken once. The sweep looks at hundreds of thousands of words, and asking
// the kernel about each one is the difference between a search and a freeze -
// the first version did exactly that and hung the game on the loading screen.
const asi::mem::Region* RegionFor(const std::vector<asi::mem::Region>& regions,
                                  std::uintptr_t address) {
  std::size_t low = 0;
  std::size_t high = regions.size();
  while (low < high) {
    const std::size_t mid = (low + high) / 2;
    if (address < regions[mid].base) {
      high = mid;
    } else if (address >= regions[mid].base + regions[mid].size) {
      low = mid + 1;
    } else {
      return &regions[mid];
    }
  }
  return nullptr;
}

// Every pointer samp.dll holds that leads somewhere big enough and texty
// enough to be a chat log. Bounded twice over - by a count and by a clock -
// because this runs inside a frame.
std::vector<Candidate> SweepRoots(const asi::mem::Module& samp,
                                  unsigned long long deadline) {
  std::vector<Candidate> found;
  const std::vector<asi::mem::Region> everything = asi::mem::ReadableRegions();
  const std::vector<asi::mem::Region> data = asi::mem::ReadableRegions(&samp);

  std::vector<std::uint32_t> chunk(kSweepWords);
  for (const asi::mem::Region& region : data) {
    if (!region.is_writable) continue;  // a root pointer lives in writable data
    const std::size_t count = region.size / sizeof(std::uint32_t);
    std::size_t have = 0;
    std::size_t from = 0;

    for (std::size_t i = 0; i < count; ++i) {
      if (found.size() >= static_cast<std::size_t>(kMaxRoots)) return found;
      if ((i & 0x3FF) == 0 && GetTickCount64() > deadline) return found;
      if (i >= from + have) {
        from = i;
        const std::size_t want =
            (count - from < kSweepWords ? count - from : kSweepWords) *
            sizeof(std::uint32_t);
        have = asi::mem::ReadGuarded(region.base + from * sizeof(std::uint32_t),
                                     chunk.data(), want) /
               sizeof(std::uint32_t);
        if (have == 0) break;
      }

      const std::uintptr_t value = chunk[i - from];
      if (value < 0x00010000u || value >= 0xC0000000u) continue;
      if (value % 4 != 0) continue;

      const asi::mem::Region* target = RegionFor(everything, value);
      if (target == nullptr) continue;
      if (!target->is_private || !target->is_writable) continue;
      const std::size_t span = target->base + target->size - value;
      if (span < kMinBlockSpan) continue;

      bool already = false;
      for (const Candidate& seen : found)
        if (seen.block == value) { already = true; break; }
      if (already) continue;

      if (CountFieldStarts(reinterpret_cast<const unsigned char*>(value), span,
                           kFilterScan, kMinBlockStrings) < kMinBlockStrings)
        continue;

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
void Describe(const Candidate& candidate, const Shape& shape,
              ChatLayout* layout) {
  const auto* block = reinterpret_cast<const unsigned char*>(candidate.block);

  Column columns[kMaxColumns];
  const int column_count =
      ScanColumns(block, candidate.span, shape, columns, kMaxColumns);

  // The message is the column that holds sentences. The reference offset is
  // simply wherever the first field start landed, which inside an entry is as
  // likely to be the speaker as the text, so it gets no special standing -
  // except as a tie-breaker, because the reference is the first text in the
  // block and so the column the array actually starts at. Taking a tied
  // column at some other offset instead shifts every read by an entry and
  // drags in whatever sat in memory before the array.
  const Column* message = nullptr;
  for (int i = 0; i < column_count; ++i) {
    if (message == nullptr || columns[i].sentences > message->sentences ||
        (columns[i].sentences == message->sentences &&
         columns[i].total_length > message->total_length) ||
        (columns[i].sentences == message->sentences &&
         columns[i].total_length == message->total_length &&
         Magnitude(columns[i].delta) < Magnitude(message->delta)))
      message = &columns[i];
  }

  const std::int32_t text_delta = message ? message->delta : 0;
  const int message_average =
      message && message->hits > 0 ? message->total_length / message->hits : 0;

  // The speaker, if the entries have one. Two tests, both learned the hard
  // way from a panel that printed every line beside the one before it:
  //
  //   - A column a whole stride away is not another column. It is this same
  //     column in the next entry, and it matches on every count there is.
  //   - Whoever is speaking is named in fewer characters than they used to
  //     say something. A copy of the message column averages exactly what the
  //     message column averages, so this rules that out too.
  const Column* speaker = nullptr;
  for (int i = 0; i < column_count; ++i) {
    const std::int32_t gap = columns[i].delta - text_delta;
    if (gap > -4 && gap < 4) continue;
    if (gap <= -static_cast<std::int32_t>(shape.stride) ||
        gap >= static_cast<std::int32_t>(shape.stride))
      continue;
    if (columns[i].hits == 0) continue;
    if (columns[i].total_length / columns[i].hits >= message_average) continue;
    // And it has to hold names. Without this the colour of each line wins:
    // four bytes, shorter than any message, printable often enough to pass.
    if (columns[i].namelike * 2 < columns[i].hits) continue;
    if (speaker == nullptr || columns[i].hits > speaker->hits ||
        (columns[i].hits == speaker->hits &&
         columns[i].total_length > speaker->total_length))
      speaker = &columns[i];
  }

  layout->block        = candidate.block;
  layout->span         = candidate.span;
  layout->root_rva     = candidate.rva;
  layout->stride       = shape.stride;
  layout->entries      = shape.count;
  // Counted on the column that gets read, not on the one the search anchored
  // on: with the two a hundred and twenty six bytes apart, they disagreed.
  const std::int64_t text_at =
      static_cast<std::int64_t>(shape.reference) + text_delta;
  layout->populated =
      text_at >= 0 ? CountStrings(block, candidate.span,
                                  static_cast<std::size_t>(text_at),
                                  shape.stride, shape.count)
                   : shape.strings;
  layout->sentences    = shape.sentences;
  layout->first_text   = candidate.block + shape.reference + text_delta;
  layout->has_prefix   = speaker != nullptr;
  layout->prefix_delta = speaker ? speaker->delta - text_delta : 0;

  std::int32_t time_delta = 0;
  int  time_base = kClockTicks;
  bool newest_first = false;
  if (ScanClock(block, candidate.span, shape, GetTickCount(), NowSeconds(),
                &time_delta, &time_base, &newest_first)) {
    layout->has_time  = true;
    layout->time_base = time_base;
    // Recorded against the message column, so a reader never has to know
    // where the entry formally begins.
    layout->time_delta   = time_delta - text_delta;
    layout->newest_first = newest_first;
    layout->order_known  = true;
  }
}

bool TryCandidate(const Candidate& candidate, std::size_t scan,
                  int max_references, ChatLayout* layout) {
  Shape shape;
  if (!SearchRing(reinterpret_cast<const unsigned char*>(candidate.block),
                  candidate.span, scan, max_references, &shape))
    return false;
  Describe(candidate, shape, layout);
  return true;
}

// Does this ring hold the line the client printed when it connected? That
// line contains the address the launcher was given, which is the one piece of
// chat we can prove exists without looking at the screen.
bool ContainsHost(const ChatLayout& layout, const std::string& host) {
  if (host.empty()) return false;
  for (int i = 0; i < layout.entries; ++i) {
    const std::string text =
        ReadField(layout.first_text +
                  static_cast<std::uintptr_t>(i) * layout.stride);
    if (text.find(host) != std::string::npos) return true;
  }
  return false;
}

// The anchored search. Every SA-MP client prints "Connecting to <address>"
// into the chat before it joins, and we know that address for certain: it came
// off the launcher command line and was confirmed against CNetGame. So a copy
// of it that is neither the command line nor CNetGame is a line of chat, and
// the array it sits in is the chat log - no shape argument required.
bool SearchByConnectLine(const std::string& host, unsigned long long deadline,
                         ChatLayout* layout) {
  if (host.empty()) return false;

  // Private regions only. The chat log is a heap allocation, and skipping
  // everything the process merely mapped - images, textures, fonts - takes
  // most of the address space out of the scan without taking anything that
  // could hold the line we are after.
  std::vector<asi::mem::Region> regions;
  for (const asi::mem::Region& region : asi::mem::ReadableRegions())
    if (region.is_private && region.is_writable) regions.push_back(region);
  const std::vector<asi::mem::Hit> hits =
      asi::mem::Scan(regions, host, kMaxAnchorHits, kScanBudget);

  for (const asi::mem::Hit& hit : hits) {
    if (GetTickCount64() > deadline) break;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(hit.address), &mbi,
                      sizeof(mbi)))
      continue;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) continue;

    Candidate candidate;
    candidate.block = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    candidate.span  = ReadableSpan(candidate.block, kBlockScan);
    candidate.rva   = 0;
    if (hit.address < candidate.block) continue;
    if (hit.address - candidate.block >= candidate.span) continue;

    const auto* block = reinterpret_cast<const unsigned char*>(candidate.block);
    // The address sits in the middle of the line; the field starts wherever
    // the printable run before it began.
    const std::size_t at =
        FieldStartBefore(block, hit.address - candidate.block);

    int confirmations = 0;
    for (std::uint32_t stride = kMinStride; stride <= kMaxStride; stride += 4) {
      if (confirmations >= kMaxConfirmations) break;
      std::size_t first = at;
      int count = 0;
      ExpandRun(block, candidate.span, at, stride, &first, &count);
      if (count < kMinRun) continue;

      Shape shape;
      shape.reference = static_cast<std::uint32_t>(first);
      shape.stride    = stride;
      shape.count     = count;
      shape.strings =
          CountStrings(block, candidate.span, first, stride, count);
      shape.sentences =
          CountSentences(block, candidate.span, first, stride, count);
      if (shape.strings < kMinStrings || shape.sentences < kMinSentences)
        continue;

      ChatLayout attempt;
      Describe(candidate, shape, &attempt);
      ++confirmations;
      // The stride is only right if the connect line is still in the column
      // the description settled on. A stride that is a multiple of the true
      // one passes every test above and reads every other line.
      if (!ContainsHost(attempt, host)) continue;
      attempt.valid    = true;
      attempt.anchored = true;
      attempt.note =
          "chat found by the line the client printed when it connected";
      *layout = attempt;
      return true;
    }
  }
  return false;
}

}  // namespace

const ChatLayout& CachedChat() { return g_layout; }

void ForgetChat() {
  g_resolved = false;
  g_layout   = ChatLayout{};
  g_anchored_attempts = 0;
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

  // Nothing runs until the client is actually in a server. There is no chat to
  // find before that, and searching for it anyway is what hung the game on the
  // loading screen - the sweep has no reason to be cheap when it is guaranteed
  // to find nothing.
  const Layout& client = ResolveLayout();
  if (!client.valid) {
    layout.note = "waiting to be in a server: " + client.note;
    g_layout    = layout;
    return g_layout;
  }
  const std::string host = client.host;
  const unsigned long long deadline = GetTickCount64() + kResolveBudgetMs;

  // The documented pointer first. It costs one read to try, and when it is
  // right nothing else has to run.
  std::uint32_t documented = 0;
  if (asi::mem::Read<std::uint32_t>(samp.base + kChatPointerRva, &documented) &&
      IsHeapPointer(documented)) {
    Candidate candidate;
    candidate.block = documented;
    candidate.span  = ReadableSpan(documented, kBlockScan);
    candidate.rva   = kChatPointerRva;
    layout.roots_tried = 1;
    if (candidate.span >= kMinBlockSpan &&
        TryCandidate(candidate, kBlockScan, kReferencesPrimary, &layout)) {
      layout.valid = true;
      layout.note  = "chat found through the documented pointer";
    }
  }

  if (!layout.valid) {
    const std::vector<Candidate> roots = SweepRoots(samp, deadline);
    layout.roots_tried += static_cast<int>(roots.size());
    int searched = 0;
    for (const Candidate& candidate : roots) {
      if (++searched > kMaxSearches) break;
      if (GetTickCount64() > deadline) break;
      ChatLayout attempt;
      if (!TryCandidate(candidate, kSweepScan, kReferencesFallback, &attempt))
        continue;
      // The best of the sweep, not the first: several blocks hold text at a
      // regular stride, and the chat log is the one full of sentences.
      if (attempt.sentences <= layout.sentences) continue;
      attempt.valid = true;
      attempt.note  = "chat found by sweeping samp.dll's pointers - the "
                      "documented offset did not lead to it";
      const int tried = layout.roots_tried;
      layout = attempt;
      layout.roots_tried = tried;
    }
  }

  // Whatever the shape search settled on, the connect line decides whether it
  // is really the chat. A ring of sentences somewhere else in the client is
  // not impossible, and this is the one line we can prove belongs here.
  if (layout.valid && ContainsHost(layout, host)) layout.anchored = true;

  if ((!layout.valid || !layout.anchored) &&
      g_anchored_attempts < kMaxAnchoredAttempts && !host.empty()) {
    ++g_anchored_attempts;
    ChatLayout anchored;
    if (SearchByConnectLine(host, deadline + kAnchorBudgetMs, &anchored)) {
      anchored.roots_tried = layout.roots_tried;
      layout = anchored;
    }
  }

  if (!layout.valid) {
    layout.note = "no ring of chat lines found in " +
                  std::to_string(layout.roots_tried) +
                  " candidate blocks - connect to a server and let some chat "
                  "arrive first";
    g_layout = layout;
    return g_layout;  // keep trying; the chat fills up after joining
  }

  g_layout   = layout;
  g_resolved = true;
  LOG_INFO("chat resolved: block=0x{:08X} via samp.dll+0x{:X} stride={} "
           "entries={} lines={} sentences={} anchored={} order={}",
           layout.block, layout.root_rva, layout.stride, layout.entries,
           layout.populated, layout.sentences, layout.anchored,
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

  char source[32];
  std::snprintf(source, sizeof(source), "samp.dll+0x%X", layout.root_rva);
  out["stride"]   = layout.stride;
  out["entries"]  = layout.entries;
  out["anchored"] = layout.anchored;
  out["source"]   = layout.root_rva ? std::string(source)
                                    : std::string("found by the connect line");
  out["order"] = layout.order_known
                     ? (layout.newest_first ? "newest first" : "oldest first")
                     : "unknown";

  const std::uint32_t now_ticks   = GetTickCount();
  const std::uint32_t now_seconds = NowSeconds();
  std::vector<json> lines;
  int stale = 0;
  lines.reserve(static_cast<std::size_t>(layout.entries));
  for (int i = 0; i < layout.entries; ++i) {
    const std::uintptr_t at =
        layout.first_text + static_cast<std::uintptr_t>(i) * layout.stride;
    const std::string text = ReadField(at);
    if (text.empty()) continue;

    // A slot the client has not written yet still holds whatever the
    // allocator left in it, and some of that reads as text. When there is a
    // clock column, it says which slots are lines and which are leftovers -
    // the only test that tells them apart, since a real line is allowed to be
    // short and meaningless too.
    std::int64_t age_ms = -1;
    if (layout.has_time) {
      std::uint32_t stamp = 0;
      if (!asi::mem::Read<std::uint32_t>(at + layout.time_delta, &stamp) ||
          !OnClock(stamp, layout.time_base, now_ticks, now_seconds)) {
        ++stale;
        continue;
      }
      age_ms = layout.time_base == kClockTicks
                   ? static_cast<std::int64_t>(now_ticks - stamp)
                   : static_cast<std::int64_t>(now_seconds - stamp) * 1000;
    }

    json line;
    line["text"] = ToUtf8(text);
    if (layout.has_prefix) {
      const std::string from = ReadField(at + layout.prefix_delta);
      if (!from.empty()) line["from"] = ToUtf8(from);
    }
    if (age_ms >= 0) line["age_ms"] = age_ms;
    lines.push_back(std::move(line));
  }
  out["stale_slots"] = stale;

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

  const std::string speaker =
      layout.has_prefix ? std::to_string(layout.prefix_delta) + " bytes from it"
                        : std::string("none found");
  const std::string stamp =
      layout.has_time
          ? std::to_string(layout.time_delta) + " bytes from it, " +
                (layout.time_base == kClockTicks ? "ticks since boot"
                                                 : "seconds since 1970")
          : std::string("none found");
  const std::string order =
      layout.order_known
          ? std::string(layout.newest_first ? "newest first" : "oldest first")
          : std::string("unknown - no timestamp column to settle it");

  char header[384];
  std::snprintf(header, sizeof(header),
                "block      0x%08X  (%u bytes readable)\n"
                "pointer    samp.dll+0x%X\n"
                "stride     %u bytes\n"
                "entries    %d, %d holding a line, %d reading as a sentence\n"
                "anchored   %s\n"
                "message    at the reference column\n"
                "speaker    %s\n"
                "timestamp  %s\n"
                "order      %s\n",
                static_cast<unsigned>(layout.block),
                static_cast<unsigned>(layout.span), layout.root_rva,
                layout.stride, layout.entries, layout.populated,
                layout.sentences,
                layout.anchored ? "yes - the connect line is in it"
                                : "no - shape only",
                speaker.c_str(), stamp.c_str(), order.c_str());
  file << header << "\n";

  // The raw bytes of the last few entries, so a column this pass got wrong
  // can be read off by hand rather than guessed at again.
  constexpr int kDumpEntries = 8;
  const int from =
      layout.entries > kDumpEntries ? layout.entries - kDumpEntries : 0;
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
        ascii[b] =
            (value >= 0x20 && value < 0x7F) ? static_cast<char>(value) : '.';
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
