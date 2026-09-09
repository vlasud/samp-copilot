#include "samp/bubbles.hpp"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>

#include "samp/version.hpp"
#include "types.hpp"

namespace gtabot::samp {
namespace {

// The search is bounded so that a mistyped string cannot walk the whole
// address space while the game is trying to draw a frame.
constexpr std::size_t kMostRegions = 4000;
constexpr std::size_t kBiggestRegion = 64u * 1024u * 1024u;
constexpr std::size_t kAround = 24;

std::string g_note = "not searched yet";

// Which module an address belongs to, if any.
std::string Whose(std::uintptr_t at) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(at), &module) ||
      module == nullptr)
    return "heap";
  char path[MAX_PATH] = {};
  if (GetModuleFileNameA(module, path, MAX_PATH) == 0) return "a module";
  const char* name = std::strrchr(path, 0x5C);
  name = name ? name + 1 : path;
  char text[160];
  std::snprintf(text, sizeof(text), "%s+0x%X", name,
                static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(module)));
  return text;
}

// The bytes on either side, printable ones kept, the rest as dots. Enough to
// recognise a record - a name beside the words, a length in front of them.
std::string Around(const std::uint8_t* start, std::size_t at, std::size_t size,
                   std::size_t length) {
  const std::size_t from = at > kAround ? at - kAround : 0;
  const std::size_t to = at + length + kAround < size ? at + length + kAround : size;
  std::string out;
  for (std::size_t i = from; i < to; ++i) {
    const std::uint8_t byte = start[i];
    if (i == at) out += " >>";
    if (byte >= 0x20 && byte != 0x7F) out += static_cast<char>(byte);
    else out += '.';
    if (i + 1 == at + length) out += "<< ";
  }
  return out;
}

// The other way round from ToUtf8: what the client would actually hold.
//
// Everything a Russian server says is CP1251, one byte a letter. Searching
// for the UTF-8 the tool is handed finds only this module's own JSON buffers
// - which is exactly what the first search found, every hit sitting beside
// "capabilities" and "content". So the query is looked for both ways.
std::string ToCp1251(const std::string& utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (std::size_t i = 0; i < utf8.size();) {
    const auto lead = static_cast<unsigned char>(utf8[i]);
    if (lead < 0x80) { out += static_cast<char>(lead); ++i; continue; }
    if (i + 1 >= utf8.size()) break;
    const auto next = static_cast<unsigned char>(utf8[i + 1]);
    const unsigned code = ((lead & 0x1Fu) << 6) | (next & 0x3Fu);
    if (code >= 0x410 && code <= 0x44F)
      out += static_cast<char>(0xC0 + (code - 0x410));      // А-я
    else if (code == 0x401)
      out += static_cast<char>(0xA8);                       // Ё
    else if (code == 0x451)
      out += static_cast<char>(0xB8);                       // ё
    else
      out += '?';
    i += 2;
  }
  return out;
}

// Bytes this module is done with, so that the next search does not find
// them lying about in freed memory and report itself.
void Wipe(std::string* text) {
  if (text != nullptr && !text->empty())
    std::memset(&(*text)[0], 0, text->size());
  if (text != nullptr) text->clear();
}

// The scan, with nothing in it that has to be destroyed, so a guard is
// allowed round it.
std::size_t ScanRegion(const std::uint8_t* start, std::size_t size,
                       const char* want, std::size_t length,
                       std::size_t* hits, std::size_t room) {
  std::size_t got = 0;
  if (room == 0) return 0;
  const std::uint8_t first = static_cast<std::uint8_t>(want[0]);
  __try {
    for (std::size_t i = 0; i + length <= size; ++i) {
      if (start[i] != first) continue;
      if (std::memcmp(start + i, want, length) != 0) continue;
      hits[got++] = i;
      if (got >= room) break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return got;
  }
  return got;
}

}  // namespace

std::vector<Found> FindText(const std::string& text, std::size_t max) {
  std::vector<Found> found;
  if (text.size() < 3) {
    g_note = "give it at least three characters, or everything matches";
    return found;
  }

  // Russian words are looked for as CP1251 and nothing else.
  //
  // The client holds the server's text one byte a letter; the only UTF-8
  // copies of a Russian phrase anywhere in this process are this module's
  // own JSON, the very buffers holding the question being asked. Searching
  // for both fills the answer with self-portraits and the real copies never
  // fit under the limit. An English query converts to itself, so it is
  // simply searched once.
  // The one copy of the query this module is allowed to hold.
  //
  // Converting the question puts the answer into the heap, and the search
  // then finds it and reports the searcher to itself - which is what the
  // first attempts kept turning up, hits sitting beside the name of the
  // pipe this conversation runs over. A string that always lives in the
  // same place can be skipped by address, and reusing it leaves no freed
  // copies behind for the next search to trip over.
  static char g_wanted[512];
  const char* encoding = "utf-8";
  std::string prepared = text;
  std::string ansi = ToCp1251(text);
  if (ansi != text && ansi.size() >= 3 && ansi.find('?') == std::string::npos) {
    prepared = ansi;
    encoding = "cp1251";
  }
  if (prepared.size() >= sizeof(g_wanted)) {
    g_note = "that is too long to look for";
    return found;
  }
  const std::size_t length = prepared.size();
  std::memset(g_wanted, 0, sizeof(g_wanted));
  std::memcpy(g_wanted, prepared.data(), length);
  // Every other copy this module made is rubbed out before the search
  // starts. Freeing them is not enough - the bytes stay where they were and
  // the very next pass finds them, which is how the first attempts came back
  // holding nothing but pictures of themselves.
  Wipe(&prepared);
  Wipe(&ansi);
  const auto ours_from = reinterpret_cast<std::uintptr_t>(g_wanted);
  const auto ours_to = ours_from + sizeof(g_wanted);
  std::size_t ours = 0;

  const HANDLE self = GetCurrentProcess();
  std::uintptr_t at = 0;
  std::size_t regions = 0, searched = 0;
  MEMORY_BASIC_INFORMATION region{};
  while (regions < kMostRegions &&
         VirtualQuery(reinterpret_cast<LPCVOID>(at), &region, sizeof(region)) ==
             sizeof(region)) {
    at = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    ++regions;
    if (region.State != MEM_COMMIT) continue;
    if ((region.Protect & PAGE_GUARD) || (region.Protect & PAGE_NOACCESS)) continue;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY | PAGE_READONLY;
    if ((region.Protect & writable) == 0) continue;
    if (region.RegionSize > kBiggestRegion) continue;

    const auto* start = static_cast<const std::uint8_t*>(region.BaseAddress);
    const std::size_t size = region.RegionSize;
    searched += size;
    // The scan itself keeps no objects, so it can sit under a guard; what is
    // done with a hit does, so that happens outside one. The streamer is
    // unmapping things while this runs and a committed page can stop being
    // one between the query and the read.
    std::size_t hits[64];
    const std::size_t room = max - found.size() < 64 ? max - found.size() : 64;
    const std::size_t got = ScanRegion(start, size, g_wanted, length, hits, room);
    for (std::size_t h = 0; h < got; ++h) {
      Found one;
      one.at = reinterpret_cast<std::uintptr_t>(start + hits[h]);
      // The one place this module holds the query is not a finding.
      if (one.at >= ours_from && one.at < ours_to) {
        ++ours;
        continue;
      }
      one.where = Whose(one.at);
      // The bytes round a hit are whatever the client had there - CP1251
      // text, or none at all - so they go through the same decoder as the
      // rest of the server's words before anybody makes JSON of them.
      std::string raw = Around(start, hits[h], size, length);
      one.around = ToUtf8(raw);
      Wipe(&raw);
      one.encoding = encoding;
      found.push_back(std::move(one));
    }
    if (found.size() >= max) break;
  }

  char note[200];
  std::snprintf(note, sizeof(note),
                "%d places hold that text as %s; %d regions and %.1f MB looked "
                "at; %d copies of the question itself passed over",
                static_cast<int>(found.size()), encoding,
                static_cast<int>(regions), searched / (1024.0 * 1024.0),
                static_cast<int>(ours));
  g_note = note;
  return found;
}

std::string FindTextNote() { return g_note; }

}  // namespace gtabot::samp
