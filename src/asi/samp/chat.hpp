#pragma once
//
// Reads the client's chat log.
//
// Everything the server says arrives as text and is kept by the client in a
// fixed array of entries, so the chat is readable without touching the
// network layer at all. What is not knowable in advance is where that array
// is and how wide one entry is - both differ between builds, and guessing
// either produces a column of plausible-looking rubbish.
//
// So neither is guessed. The array is recognised by its shape: a column of
// null-terminated strings repeating at a constant stride, dozens deep, which
// nothing else in the client looks like. Once the stride is known the fields
// beside the message - who said it, when - are found by testing every offset
// within one entry against every entry, and keeping the ones that hold text
// (or a plausible tick count) in most of them.
//
// Two consequences worth stating:
//
//   - Which column is the message and which is the speaker is decided by
//     evidence, not by declaration order: the message is the field present in
//     more entries and longer on average. On a roleplay server most lines are
//     the server talking, with no speaker at all, so the gap is wide.
//   - Whether index 0 is the oldest line or the newest is decided by the
//     timestamps, when a timestamp column is found. Without one the order is
//     reported as unknown rather than assumed.
//
#include <cstdint>
#include <string>

#include "types.hpp"

namespace gtabot::samp {

struct ChatLayout {
  bool           valid = false;
  // The structure holding the ring, and where in samp.dll the pointer to it
  // was found. A run of zeroes here means the fallback sweep found it.
  std::uintptr_t block    = 0;
  std::size_t    span     = 0;
  std::uint32_t  root_rva = 0;
  // The message field of the first entry that holds text. Entry i is this
  // plus i * stride; the other columns are a signed distance from it, which
  // sidesteps having to work out where an entry formally begins.
  std::uintptr_t first_text = 0;
  std::uint32_t  stride     = 0;
  int            entries    = 0;
  int            populated  = 0;
  // The speaker, when the entries have one.
  bool           has_prefix   = false;
  std::int32_t   prefix_delta = 0;
  // A tick count, when one is there. It is what settles the ordering.
  bool           has_time     = false;
  std::int32_t   time_delta   = 0;
  bool           newest_first = false;
  bool           order_known  = false;
  // How many candidate blocks were tried before this one, so a failure says
  // whether it looked in the wrong place or found nothing anywhere.
  int            roots_tried = 0;
  std::string    note;
};

// Resolved once and cached. Game thread only.
const ChatLayout& ResolveChat();

// What resolution last established, without attempting it. The search walks a
// lot of memory, and the one place it must never run is inside a draw call.
const ChatLayout& CachedChat();

// Re-runs resolution from scratch.
void ForgetChat();

// The chat log, oldest first, at most `limit` lines. Game thread only.
json ReadChat(int limit);

// Writes bot.chat-dump.txt: the shape that was found, every column that was
// considered, and the raw bytes of the last few entries. Game thread only.
bool DumpChat();

}  // namespace gtabot::samp
