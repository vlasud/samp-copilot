#include "samp/world.hpp"

#include <windows.h>

#include <string>
#include <vector>

#include "log.hpp"
#include "samp/version.hpp"
#include "state/memory.hpp"

namespace gtabot::samp {
namespace {

// *(samp.dll + this) is CNetGame*, per the public SAMP-API headers for
// 0.3.7-R1. Every other offset below is found rather than assumed.
constexpr std::uint32_t kNetGamePointer = 0x21A0F8;
// CNetGame begins with 32 bytes of padding, then the host address as text.
constexpr std::uint32_t kHostAddress = 0x20;

constexpr int kMaxPlayers = 1004;
// CNetGame::Pools is nine pointers: actor, object, gang zone, label, textdraw,
// menu, player, vehicle, pickup.
constexpr int kPoolCount   = 9;
constexpr int kPlayerPoolIndex = 6;
// How far into CNetGame to look for the Pools pointer. It is the last member,
// and the struct is a little over 0x3C0 bytes.
constexpr std::uint32_t kPoolsSearchFrom = 0x300;
constexpr std::uint32_t kPoolsSearchTo   = 0x460;
// CPlayerPool starts with the largest id and the local player's own details,
// so the arrays begin somewhere in the first few dozen bytes.
constexpr std::uint32_t kArraySearchTo = 0x80;

Layout g_layout;
bool   g_resolved = false;
// Resolution sweeps a few thousand slots looking for the arrays. Until it
// succeeds it would otherwise re-run on every frame the panel draws, so
// failed attempts back off.
unsigned long long g_last_attempt_ms = 0;
constexpr unsigned long long kRetryAfterMs = 2000;

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

// MSVC keeps a short string inside the object and a long one behind a pointer,
// with the size and capacity after the buffer. Which offset those sit at
// depends on the toolset SA-MP was built with, so both are tried and the one
// that reads back sensibly is the one used from then on.
bool ReadStdString(std::uintptr_t address, int variant, std::string* out) {
  const std::uintptr_t buffer = address + (variant == 0 ? 0 : 4);
  std::uint32_t size = 0;
  std::uint32_t capacity = 0;
  if (!asi::mem::Read<std::uint32_t>(buffer + 16, &size)) return false;
  if (!asi::mem::Read<std::uint32_t>(buffer + 20, &capacity)) return false;
  if (size > capacity || capacity > 4096 || size > 256) return false;

  std::uintptr_t chars = buffer;
  if (capacity >= 16) {
    std::uint32_t pointer = 0;
    if (!asi::mem::Read<std::uint32_t>(buffer, &pointer)) return false;
    if (!IsHeapPointer(pointer)) return false;
    chars = pointer;
  }
  if (!asi::mem::IsReadable(chars, size + 1)) return false;

  std::string value;
  value.reserve(size);
  for (std::uint32_t i = 0; i < size; ++i) {
    const char c = *reinterpret_cast<const char*>(chars + i);
    // A name is text. Anything else means this is not a string at all.
    if (static_cast<unsigned char>(c) < 0x20) return false;
    value.push_back(c);
  }
  *out = std::move(value);
  return true;
}

// The pool holds 1004 CPlayerInfo pointers - null for empty slots - followed by
// 1004 flags that are only ever 0 or 1. Nothing else in the structure looks
// like that, which is what makes it findable without a computed offset.
bool LooksLikeSlotArrays(std::uintptr_t pool, std::uint32_t offset) {
  const std::uintptr_t objects   = pool + offset;
  const std::uintptr_t not_empty = objects + kMaxPlayers * 4;
  if (!asi::mem::IsReadable(objects, kMaxPlayers * 4 * 2)) return false;

  const auto* object_values = reinterpret_cast<const std::uint32_t*>(objects);
  const auto* flag_values   = reinterpret_cast<const std::uint32_t*>(not_empty);

  int occupied = 0;
  for (int i = 0; i < kMaxPlayers; ++i) {
    if (flag_values[i] > 1) return false;
    const bool present = flag_values[i] == 1;
    if (present) {
      if (!IsHeapPointer(object_values[i])) return false;
      ++occupied;
    }
  }
  // An all-empty pool is indistinguishable from a run of zeroes.
  return occupied > 0;
}

std::string CommandLineHost() {
  const std::string line = GetCommandLineA();
  const std::size_t at = line.find("-h ");
  if (at == std::string::npos) return {};
  std::size_t begin = at + 3;
  while (begin < line.size() && line[begin] == ' ') ++begin;
  std::size_t end = line.find(' ', begin);
  if (end == std::string::npos) end = line.size();
  return line.substr(begin, end - begin);
}

}  // namespace

void ForgetLayout() {
  g_resolved = false;
  g_layout   = Layout{};
}

const Layout& ResolveLayout() {
  if (g_resolved) return g_layout;
  const unsigned long long now = GetTickCount64();
  if (g_last_attempt_ms != 0 && now - g_last_attempt_ms < kRetryAfterMs)
    return g_layout;
  g_last_attempt_ms = now;

  Layout layout;
  const asi::mem::Module samp = asi::mem::FindModule(L"samp.dll");
  if (!samp.valid()) {
    layout.note = "samp.dll is not loaded";
    g_layout = layout;
    g_resolved = true;
    return g_layout;
  }

  std::uint32_t net_game = 0;
  if (!asi::mem::Read<std::uint32_t>(samp.base + kNetGamePointer, &net_game) ||
      !IsHeapPointer(net_game)) {
    layout.note = "no CNetGame at samp.dll+0x21A0F8 yet - not connected";
    g_layout = layout;
    g_resolved = false;  // keep trying; it appears on connect
    return g_layout;
  }
  layout.net_game = net_game;

  // The anchor. If the text at +0x20 is the address the launcher was told to
  // connect to, the pointer and the start of the structure are both right.
  layout.host = asi::mem::ReadCString(net_game + kHostAddress, 64);
  const std::string expected = CommandLineHost();
  if (layout.host.empty()) {
    layout.note = "CNetGame candidate has no host address at +0x20";
    g_layout = layout;
    g_resolved = false;
    return g_layout;
  }
  if (!expected.empty() && layout.host != expected) {
    layout.note = "host address at +0x20 is '" + layout.host + "' but the "
                  "launcher was given '" + expected + "' - not the structure "
                  "we think it is";
    g_layout = layout;
    g_resolved = true;
    return g_layout;
  }

  // Pools: nine consecutive heap pointers, at the tail of CNetGame.
  for (std::uint32_t offset = kPoolsSearchFrom; offset < kPoolsSearchTo;
       offset += 4) {
    std::uint32_t candidate = 0;
    if (!asi::mem::Read<std::uint32_t>(net_game + offset, &candidate)) continue;
    if (!IsHeapPointer(candidate)) continue;
    if (!asi::mem::IsReadable(candidate, kPoolCount * 4)) continue;

    const auto* entries = reinterpret_cast<const std::uint32_t*>(candidate);
    bool all_pools = true;
    for (int i = 0; i < kPoolCount && all_pools; ++i)
      all_pools = IsHeapPointer(entries[i]);
    if (!all_pools) continue;

    const std::uintptr_t player_pool = entries[kPlayerPoolIndex];
    for (std::uint32_t inner = 0; inner < kArraySearchTo; inner += 4) {
      if (!LooksLikeSlotArrays(player_pool, inner)) continue;
      layout.pools           = candidate;
      layout.player_pool     = player_pool;
      layout.object_array    = inner;
      layout.not_empty_array = inner + kMaxPlayers * 4;
      break;
    }
    if (layout.player_pool) break;
  }

  if (!layout.player_pool) {
    layout.note = "found CNetGame (host " + layout.host +
                  ") but no player pool - nobody is in the pool yet";
    g_layout = layout;
    g_resolved = false;
    return g_layout;
  }

  // CPlayerPool begins with the largest id and then the local player's own
  // record, whose name is the first std::string in the structure.
  for (int variant = 0; variant < 2 && layout.string_variant < 0; ++variant) {
    std::string name;
    if (ReadStdString(layout.player_pool + 0x0C, variant, &name) && !name.empty())
      layout.string_variant = variant;
  }
  if (layout.string_variant < 0) {
    layout.note = "player pool found, but the local player's name does not "
                  "read as a std::string in either layout";
    g_layout = layout;
    g_resolved = true;
    return g_layout;
  }

  layout.valid = true;
  layout.note  = "resolved";
  g_layout     = layout;
  g_resolved   = true;

  LOG_INFO("SA-MP layout resolved: CNetGame=0x{:08X} host={} pools=0x{:08X} "
           "playerPool=0x{:08X} objects=+0x{:X} string layout {}",
           layout.net_game, layout.host, layout.pools, layout.player_pool,
           layout.object_array, layout.string_variant);
  return g_layout;
}

json ReadWorld() {
  const Layout& layout = ResolveLayout();
  if (!layout.valid)
    return json{{"resolved", false}, {"note", layout.note}};

  json players = json::array();
  const auto* objects =
      reinterpret_cast<const std::uint32_t*>(layout.player_pool +
                                             layout.object_array);
  const auto* present =
      reinterpret_cast<const std::uint32_t*>(layout.player_pool +
                                             layout.not_empty_array);

  if (!asi::mem::IsReadable(reinterpret_cast<std::uintptr_t>(objects),
                            kMaxPlayers * 4 * 2))
    return json{{"resolved", false},
                {"note", "the pool moved - re-resolving next time"}};

  for (int id = 0; id < kMaxPlayers; ++id) {
    if (present[id] != 1) continue;
    const std::uintptr_t info = objects[id];

    // CPlayerInfo: CRemotePlayer*, BOOL is-NPC, alignment, then the name.
    std::uint32_t is_npc = 0;
    std::string   name;
    asi::mem::Read<std::uint32_t>(info + 0x04, &is_npc);
    if (!ReadStdString(info + 0x0C, layout.string_variant, &name)) continue;

    json entry{{"id", id}, {"name", name}, {"npc", is_npc != 0}};

    // Score and ping follow the name; their offset depends on how wide the
    // string turned out to be.
    const std::uint32_t after_name =
        0x0C + (layout.string_variant == 0 ? 24u : 28u);
    std::uint32_t score = 0;
    std::uint32_t ping = 0;
    if (asi::mem::Read<std::uint32_t>(info + after_name, &score))
      entry["score"] = static_cast<int>(score);
    if (asi::mem::Read<std::uint32_t>(info + after_name + 4, &ping))
      entry["ping"] = ping;
    players.push_back(std::move(entry));
  }

  std::string local_name;
  ReadStdString(layout.player_pool + 0x0C, layout.string_variant, &local_name);
  std::uint32_t local_id = 0;
  asi::mem::Read<std::uint32_t>(layout.player_pool + 0x04, &local_id);

  return json{
      {"resolved", true},
      {"host", layout.host},
      {"self", {{"id", local_id & 0xFFFF}, {"name", local_name}}},
      {"players", std::move(players)},
      {"player_count", players.size()},
  };
}

}  // namespace gtabot::samp
