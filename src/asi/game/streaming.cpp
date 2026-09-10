#include "game/streaming.hpp"

#include <windows.h>

#include <cstdint>
#include <set>

#include "game/exe.hpp"
#include "log.hpp"
#include "state/memory.hpp"

namespace gtabot::game::streaming {
namespace {

// GTA San Andreas 1.0 US, from the gta-reversed sources:
//
//   CColStore::ms_pColPool               0x965560   CPool<ColDef>*
//   CStreaming::RequestModel(id, flags)  0x4087E0   __cdecl
//   CStreaming::LoadAllRequestedModels   0x40EA10   __cdecl (bool priority only)
//   CStreaming::ms_memoryAvailable       0x8A5A80   size_t
//
// A collision file has a model id of its own, above the ordinary models:
// twenty-five thousand plus its slot.
constexpr std::uint32_t kColPoolPtr   = 0x965560;
constexpr std::uint32_t kRequestModel = 0x4087E0;
constexpr std::uint32_t kLoadRequested = 0x40EA10;
constexpr std::uint32_t kMemoryAvailable = 0x8A5A80;
constexpr std::uint32_t kMemoryUsed = 0x8E4CB4;
constexpr int kColModelBase = 25000;
// The path graph: sixty-four areas of nodes, streamed the same way.
constexpr int kNodeModelBase = 25511;
constexpr int kNodeAreas = 64;

// CPool: the storage, the byte per slot, the count. The byte's top bit means
// the slot is empty, as everywhere else in this game.
constexpr std::uint32_t kPoolStorage = 0x00, kPoolSlots = 0x04, kPoolCapacity = 0x08;
constexpr std::uint8_t  kSlotEmpty = 0x80;

// ColDef: a rectangle, a name, the models it covers, a reference count and
// four flags - forty-four bytes. The rectangle is left, top, right, bottom
// in memory, which in this game's naming means minX, maxY, maxX, minY.
constexpr std::uint32_t kColDefStride = 0x2C;
constexpr std::uint32_t kAreaLeft = 0x00, kAreaTop = 0x04, kAreaRight = 0x08, kAreaBottom = 0x0C;

// Pinned in memory and left there, at the front of the queue.
constexpr std::int32_t kKeepInMemory = 0x08;
constexpr std::int32_t kPriority     = 0x10;

// A slot big enough to hold the whole map's collision. The game gives itself
// far less; the difference is what makes this possible at all.
constexpr std::size_t kWantedBudget = 512u * 1024u * 1024u;
constexpr std::size_t kMostBudget   = 1024u * 1024u * 1024u;

using RequestModelFn = void(__cdecl*)(std::int32_t, std::int32_t);
using LoadRequestedFn = void(__cdecl*)(bool);

std::set<int> g_pinned;
std::set<int> g_pinned_nodes;
bool g_said_budget = false;

using game::At;

std::uintptr_t Pool() {
  std::uint32_t pool = 0;
  if (!asi::mem::Read<std::uint32_t>(At(kColPoolPtr), &pool) || pool == 0) return 0;
  if (!asi::mem::IsReadable(pool, 0x14)) return 0;
  return pool;
}

bool AskForModel(int id) {
  __try {
    reinterpret_cast<RequestModelFn>(At(kRequestModel))(
        id, kPriority | kKeepInMemory);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool AskFor(int slot) { return AskForModel(kColModelBase + slot); }

bool LoadWhatWasAsked() {
  __try {
    reinterpret_cast<LoadRequestedFn>(At(kLoadRequested))(true);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

bool Ready() {
  return Pool() != 0 &&
         asi::mem::IsReadable(At(kMemoryAvailable), sizeof(std::size_t)) &&
         asi::mem::IsReadable(At(kRequestModel), 16) &&
         asi::mem::IsReadable(At(kLoadRequested), 16);
}

std::size_t MemoryBudget() {
  std::uint32_t bytes = 0;
  if (!asi::mem::Read<std::uint32_t>(At(kMemoryAvailable), &bytes)) return 0;
  return bytes;
}

bool SetMemoryBudget(std::size_t bytes) {
  if (bytes > kMostBudget) bytes = kMostBudget;
  const std::size_t was = MemoryBudget();
  if (was >= bytes) {
    LOG_INFO("streaming: the game already allows itself {} MB of models at a "
             "time, which is room enough - leaving it alone",
             was / (1024 * 1024));
    return true;
  }
  // The game's own writable data: no page trick needed, only care.
  const std::uintptr_t where = At(kMemoryAvailable);
  if (!asi::mem::IsReadable(where, sizeof(std::uint32_t))) return false;
  __try {
    *reinterpret_cast<std::uint32_t*>(where) = static_cast<std::uint32_t>(bytes);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LOG_WARN("streaming: the game would not let its own model budget be "
             "written at {:#x} - leaving it at {} MB, so pinned collision may "
             "push the streamer over", where, was / (1024 * 1024));
    return false;
  }
  LOG_INFO("streaming: the game allowed itself {} MB of models at a time; "
           "raised to {} MB so the collision of the ground a plan needs can "
           "be held as well", was / (1024 * 1024), bytes / (1024 * 1024));
  return true;
}

int Pinned() { return static_cast<int>(g_pinned.size()); }

std::size_t MemoryUsed() {
  std::uint32_t bytes = 0;
  if (!asi::mem::Read<std::uint32_t>(At(kMemoryUsed), &bytes)) return 0;
  return bytes;
}

int PinPathNodes() {
  if (!Ready()) return 0;
  int asked = 0, wanted = 0;
  for (int area = 0; area < kNodeAreas; ++area) {
    const int id = kNodeModelBase + area;
    if (!AskForModel(id)) break;
    if (g_pinned_nodes.insert(id).second) ++asked;
    ++wanted;
  }
  if (wanted > 0) {
    LoadWhatWasAsked();
  }
  if (asked > 0) {
    LOG_INFO("streaming: asked the game for {} more of the map's {} areas of "
             "path nodes and pinned them, so a way can be worked out to "
             "somewhere the player has never been", asked, kNodeAreas);
  }
  return asked;
}

int PinWholeMap() {
  // Every area there is: the whole map's rectangle, with room to spare.
  return PinCollisionOver(-4000.0f, -4000.0f, 4000.0f, 4000.0f);
}

int PinCollisionOver(float x0, float y0, float x1, float y1) {
  if (!Ready()) return 0;
  if (!g_said_budget) {
    g_said_budget = true;
    SetMemoryBudget(kWantedBudget);
  }
  const std::uintptr_t pool = Pool();
  std::uint32_t storage = 0, slots = 0, capacity = 0;
  if (!asi::mem::Read<std::uint32_t>(pool + kPoolStorage, &storage) ||
      !asi::mem::Read<std::uint32_t>(pool + kPoolSlots, &slots) ||
      !asi::mem::Read<std::uint32_t>(pool + kPoolCapacity, &capacity))
    return 0;
  if (storage == 0 || slots == 0 || capacity == 0 || capacity > 4096) return 0;
  if (!asi::mem::IsReadable(storage, capacity * kColDefStride) ||
      !asi::mem::IsReadable(slots, capacity))
    return 0;

  if (x1 < x0) { const float swap = x0; x0 = x1; x1 = swap; }
  if (y1 < y0) { const float swap = y0; y0 = y1; y1 = swap; }

  int asked = 0, wanted = 0;
  for (std::uint32_t slot = 1; slot < capacity; ++slot) {
    std::uint8_t state = 0;
    if (!asi::mem::Read<std::uint8_t>(slots + slot, &state)) continue;
    if ((state & kSlotEmpty) != 0) continue;
    const std::uintptr_t def = storage + slot * kColDefStride;
    float left = 0, top = 0, right = 0, bottom = 0;
    if (!asi::mem::Read<float>(def + kAreaLeft, &left) ||
        !asi::mem::Read<float>(def + kAreaTop, &top) ||
        !asi::mem::Read<float>(def + kAreaRight, &right) ||
        !asi::mem::Read<float>(def + kAreaBottom, &bottom))
      continue;
    // An area that has never been given a rectangle keeps the game's own
    // empty one - a million metres the wrong way round - and would otherwise
    // match everything.
    if (right < left || top < bottom) continue;
    if (x1 < left || x0 > right || y1 < bottom || y0 > top) continue;
    if (!AskFor(static_cast<int>(slot))) break;
    // Newly wanted, or wanted again: the game removes node and collision
    // areas the player has walked away from on its own schedule, whatever
    // flag the request carried, so every one is asked for every time.
    if (g_pinned.insert(static_cast<int>(slot)).second) ++asked;
    ++wanted;
  }
  if (wanted > 0) {
    LoadWhatWasAsked();
    LOG_INFO("streaming: asked the game for the collision of {} more of the "
             "map's areas over ({:.0f},{:.0f})-({:.0f},{:.0f}); {} wanted in "
             "all, {} MB of models held of the {} MB allowed",
             asked, x0, y0, x1, y1, Pinned(), MemoryUsed() / (1024 * 1024),
             MemoryBudget() / (1024 * 1024));
  }
  return asked;
}

}  // namespace gtabot::game::streaming
