#include "samp/world.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
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
// The client's structures are packed: on this build the port sits at +0x225
// and the pools pointer at +0x3CD, neither of them on a four-byte boundary.
// Every search below therefore steps a byte at a time - stepping by four
// silently finds nothing at all.
constexpr std::uint32_t kSearchStep = 1;

constexpr int kMaxPlayers = 1004;
// CNetGame::Pools is nine pointers: actor, object, gang zone, label, textdraw,
// menu, player, vehicle, pickup.
constexpr int kPoolCount   = 9;
constexpr int kPlayerPoolIndex = 6;
// How far into CNetGame to look for the Pools pointer. It is the last member,
// and the struct is a little over 0x3C0 bytes.
constexpr std::uint32_t kPoolsSearchFrom = 0x200;
constexpr std::uint32_t kPoolsSearchTo   = 0x600;
// CPlayerPool starts with the largest id and the local player's own details,
// so the arrays begin somewhere in the first few dozen bytes.
constexpr std::uint32_t kArraySearchTo = 0x100;
// Which slot of the pool block holds the player pool. Searched rather than
// trusted: the declaration says the seventh, but a null pool ahead of it would
// shift nothing while a miscount would move everything.
constexpr int kPoolSlotsToTry = 12;

bool IsHeapPointer(std::uintptr_t value);

// From CRemotePlayer / CLocalPlayer to a position on the map.
//
// Both start with a pointer to SA-MP's own CPed wrapper, and that wrapper
// holds the game's ped at +0x2A4 - an offset the header gives away by naming
// the padding after it pad_2a8. The game's entity is a CPlaceable: a matrix
// pointer at +0x14, and a position inline at +0x04 for entities that have no
// matrix built yet.
constexpr std::uint32_t kSampPedToGamePed = 0x2A4;
constexpr std::uint32_t kEntityMatrix     = 0x14;
constexpr std::uint32_t kEntityPosition   = 0x04;
constexpr std::uint32_t kMatrixPosition   = 0x30;

// CRemotePlayer, whose front we already confirmed: a null ped here means the
// player is not streamed in, which is true of most of a 650-player server.
constexpr std::uint32_t kRemotePed   = 0x00;
constexpr std::uint32_t kRemoteVeh   = 0x04;
constexpr std::uint32_t kRemoteTeam  = 0x08;
constexpr std::uint32_t kRemoteState = 0x09;
constexpr std::uint8_t  kStatePassenger = 18;
constexpr std::uint8_t  kStateDriver    = 19;

// Offsets inside the game's own CPed, taken from plugin-sdk's declarations for
// GTA SA 1.0 US. Unlike everything above these are not SA-MP's, so they are
// checkable against the HUD: the health bar on screen is this number.
constexpr std::uint32_t kPedHealth      = 0x540;
constexpr std::uint32_t kPedMaxHealth   = 0x544;
constexpr std::uint32_t kPedArmour      = 0x548;
constexpr std::uint32_t kPedVehicle     = 0x58C;
constexpr std::uint32_t kPedWeapons     = 0x5A0;
constexpr std::uint32_t kPedWeaponSlot  = 0x718;
// CWeapon is 0x1C bytes: type, state, ammo in clip, total ammo.
constexpr std::uint32_t kWeaponStride   = 0x1C;
constexpr std::uint32_t kWeaponAmmoClip = 0x08;
constexpr std::uint32_t kWeaponAmmo     = 0x0C;
constexpr int           kWeaponSlots    = 13;

// The id is what the game stores and what the agent should reason about; the
// name is a convenience, and anything unrecognised stays unnamed rather than
// being guessed at.
const char* WeaponName(std::uint32_t id) {
  switch (id) {
    case 0:  return "fist";
    case 1:  return "brass knuckles";
    case 2:  return "golf club";
    case 3:  return "nightstick";
    case 4:  return "knife";
    case 5:  return "baseball bat";
    case 6:  return "shovel";
    case 7:  return "pool cue";
    case 8:  return "katana";
    case 9:  return "chainsaw";
    case 15: return "cane";
    case 16: return "grenade";
    case 17: return "tear gas";
    case 18: return "molotov";
    case 22: return "pistol";
    case 23: return "silenced pistol";
    case 24: return "desert eagle";
    case 25: return "shotgun";
    case 26: return "sawn-off shotgun";
    case 27: return "combat shotgun";
    case 28: return "micro smg";
    case 29: return "mp5";
    case 30: return "ak-47";
    case 31: return "m4";
    case 32: return "tec-9";
    case 33: return "rifle";
    case 34: return "sniper rifle";
    case 35: return "rpg";
    case 36: return "hs rocket";
    case 37: return "flamethrower";
    case 38: return "minigun";
    case 39: return "satchel charge";
    case 40: return "detonator";
    case 41: return "spray can";
    case 42: return "fire extinguisher";
    case 43: return "camera";
    case 44: return "night vision";
    case 45: return "thermal goggles";
    case 46: return "parachute";
    default: return nullptr;
  }
}

// CVehiclePool holds three arrays back to back: SA-MP's own wrapper, the
// in-use flags, and - the useful one - the game's vehicle. Three arrays that
// have to agree is a far stronger signature than the player pool's two, and it
// cannot be satisfied by a run of zeroes.
constexpr int kMaxVehicles = 2000;
// m_nCount plus a hundred-entry waiting list put the arrays a little over
// 0x1100 in; the window is generous because that is arithmetic, not fact.
constexpr std::uint32_t kVehicleSearchFrom = 0x0800;
constexpr std::uint32_t kVehicleSearchTo   = 0x2000;
// CEntity::m_nModelIndex, from plugin-sdk. The same CPlaceable base as a ped,
// so position comes from the matrix we already trust.
constexpr std::uint32_t kEntityModel = 0x22;

struct Position {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  bool  valid = false;
};

// San Andreas is about 6000 units across and its tallest point is under 1500.
// Anything outside that is not a position, whatever it is.
bool PlausiblePosition(float x, float y, float z) {
  const bool finite = x == x && y == y && z == z;
  return finite && x > -4000.0f && x < 4000.0f && y > -4000.0f && y < 4000.0f &&
         z > -300.0f && z < 2000.0f;
}

// The ped's own matrix is authoritative once the game has built one; until
// then the inline placement is what the entity has.
Position ReadEntityPosition(std::uintptr_t entity) {
  Position out;
  if (entity == 0) return out;

  std::uint32_t matrix = 0;
  if (asi::mem::Read<std::uint32_t>(entity + kEntityMatrix, &matrix) &&
      IsHeapPointer(matrix)) {
    float values[3] = {};
    if (asi::mem::Read<float>(matrix + kMatrixPosition, &values[0]) &&
        asi::mem::Read<float>(matrix + kMatrixPosition + 4, &values[1]) &&
        asi::mem::Read<float>(matrix + kMatrixPosition + 8, &values[2]) &&
        PlausiblePosition(values[0], values[1], values[2])) {
      out = {values[0], values[1], values[2], true};
      return out;
    }
  }

  float values[3] = {};
  if (asi::mem::Read<float>(entity + kEntityPosition, &values[0]) &&
      asi::mem::Read<float>(entity + kEntityPosition + 4, &values[1]) &&
      asi::mem::Read<float>(entity + kEntityPosition + 8, &values[2]) &&
      PlausiblePosition(values[0], values[1], values[2]))
    out = {values[0], values[1], values[2], true};
  return out;
}

// Health, armour and what the ped is holding. `full` adds the weapon's ammo,
// which is worth reporting for ourselves and noise for everyone else.
json ReadPedDetails(std::uintptr_t game_ped, bool full) {
  json out = json::object();

  float health = 0.0f;
  float max_health = 0.0f;
  float armour = 0.0f;
  // A ped's health runs 0..100 by default and servers raise the maximum, but
  // nothing legitimate is negative or in the thousands.
  if (asi::mem::Read<float>(game_ped + kPedHealth, &health) && health >= 0.0f &&
      health < 10000.0f)
    out["health"] = health;
  if (asi::mem::Read<float>(game_ped + kPedMaxHealth, &max_health) &&
      max_health > 0.0f && max_health < 10000.0f)
    out["max_health"] = max_health;
  if (asi::mem::Read<float>(game_ped + kPedArmour, &armour) && armour >= 0.0f &&
      armour < 10000.0f)
    out["armour"] = armour;

  std::uint32_t vehicle = 0;
  if (asi::mem::Read<std::uint32_t>(game_ped + kPedVehicle, &vehicle))
    out["in_vehicle"] = IsHeapPointer(vehicle);

  std::uint8_t slot = 0;
  if (asi::mem::Read<std::uint8_t>(game_ped + kPedWeaponSlot, &slot) &&
      slot < kWeaponSlots) {
    const std::uintptr_t weapon =
        game_ped + kPedWeapons + slot * kWeaponStride;
    std::uint32_t type = 0;
    if (asi::mem::Read<std::uint32_t>(weapon, &type) && type <= 46) {
      out["weapon"] = type;
      if (const char* name = WeaponName(type)) out["weapon_name"] = name;
      if (full) {
        std::uint32_t clip = 0;
        std::uint32_t ammo = 0;
        if (asi::mem::Read<std::uint32_t>(weapon + kWeaponAmmoClip, &clip))
          out["ammo_in_clip"] = clip;
        if (asi::mem::Read<std::uint32_t>(weapon + kWeaponAmmo, &ammo))
          out["ammo"] = ammo;
      }
    }
  }
  return out;
}

// samp_ped is SA-MP's wrapper; the game's entity hangs off it.
std::uint32_t GamePedOfSampPed(std::uint32_t samp_ped) {
  if (!IsHeapPointer(samp_ped)) return 0;
  std::uint32_t game_ped = 0;
  if (!asi::mem::Read<std::uint32_t>(samp_ped + kSampPedToGamePed, &game_ped))
    return 0;
  return IsHeapPointer(game_ped) ? game_ped : 0;
}

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

// The pool holds 1004 CPlayerInfo pointers - null for empty slots - alongside
// 1004 flags saying which of them are in use. The signature is the agreement
// between the two arrays, not the exact value of the flag: assuming a BOOL is
// literally 1 is the kind of detail that quietly fails.
bool LooksLikeSlotArrays(std::uintptr_t pool, std::uint32_t offset) {
  const std::uintptr_t objects   = pool + offset;
  const std::uintptr_t not_empty = objects + kMaxPlayers * 4;
  if (!asi::mem::IsReadable(objects, kMaxPlayers * 4 * 2)) return false;

  const auto* object_values = reinterpret_cast<const std::uint32_t*>(objects);
  const auto* flag_values   = reinterpret_cast<const std::uint32_t*>(not_empty);

  int occupied = 0;
  for (int i = 0; i < kMaxPlayers; ++i) {
    const bool flagged = flag_values[i] != 0;
    const bool has_object = object_values[i] != 0;
    if (flagged != has_object) return false;
    if (!flagged) continue;
    // A cheap range check for every slot; the expensive one only for a few.
    if (object_values[i] < 0x00010000u || object_values[i] >= 0xC0000000u ||
        object_values[i] % 4 != 0)
      return false;
    ++occupied;
  }
  if (occupied == 0) return false;  // indistinguishable from a run of zeroes

  int checked = 0;
  for (int i = 0; i < kMaxPlayers && checked < 4; ++i) {
    if (flag_values[i] == 0) continue;
    if (!IsHeapPointer(object_values[i])) return false;
    ++checked;
  }
  return true;
}

// Written once when the pool cannot be found, so a second attempt does not
// need another round trip to learn what is actually in the structure.
void DumpNetGame(std::uintptr_t net_game, const std::string& host) {
  const std::string path = ModuleDirectory() + "bot.netgame-dump.txt";
  std::ofstream file(path, std::ios::trunc);
  if (!file) return;

  auto describe = [](std::uint32_t value) -> std::string {
    char text[64];
    if (value == 0) return "0";
    if (IsHeapPointer(value)) {
      std::uint32_t first = 0;
      asi::mem::Read<std::uint32_t>(value, &first);
      std::snprintf(text, sizeof(text), "-> heap 0x%08X [0x%08X]", value, first);
      return text;
    }
    if (value < 100000) {
      std::snprintf(text, sizeof(text), "int %u", value);
      return text;
    }
    return "";
  };

  char header[160];
  std::snprintf(header, sizeof(header),
                "CNetGame at 0x%08X, host %s\n"
                "The player pool was not found. Every word of the structure,\n"
                "then the head of each block it points to.\n\n",
                static_cast<unsigned>(net_game), host.c_str());
  file << header;

  for (std::uint32_t offset = 0; offset < 0x600; offset += 4) {
    std::uint32_t value = 0;
    if (!asi::mem::Read<std::uint32_t>(net_game + offset, &value)) break;
    char line[128];
    std::snprintf(line, sizeof(line), "  +0x%03X  %08X  %s\n", offset, value,
                  describe(value).c_str());
    file << line;
  }

  file << "\n\nblocks pointed to from +0x200 onwards, at any alignment\n";
  for (std::uint32_t offset = 0x200; offset < 0x600; offset += 1) {
    std::uint32_t value = 0;
    if (!asi::mem::Read<std::uint32_t>(net_game + offset, &value)) break;
    if (!IsHeapPointer(value)) continue;
    if (!asi::mem::IsReadable(value, 16 * 4)) continue;

    char line[128];
    std::snprintf(line, sizeof(line), "\n  from +0x%03X -> 0x%08X\n", offset,
                  value);
    file << line;
    for (int i = 0; i < 16; ++i) {
      std::uint32_t entry = 0;
      asi::mem::Read<std::uint32_t>(value + i * 4, &entry);
      std::snprintf(line, sizeof(line), "      [%2d]  %08X  %s\n", i, entry,
                    describe(entry).c_str());
      file << line;
    }
  }
  LOG_INFO("wrote {} - CNetGame is there but the player pool was not found",
           path);
}

// Written once when the pool resolves but nobody has a ping, which means the
// scoreboard is getting those numbers from somewhere we are not looking. Rather
// than guess at another offset, this puts the bytes on the table.
void DumpPlayerInfo(const Layout& layout) {
  const std::string path = ModuleDirectory() + "bot.playerinfo-dump.txt";
  std::ofstream file(path, std::ios::trunc);
  if (!file) return;

  const auto* objects = reinterpret_cast<const std::uint32_t*>(
      layout.player_pool + layout.object_array);
  const auto* present = reinterpret_cast<const std::uint32_t*>(
      layout.player_pool + layout.not_empty_array);

  char line[200];
  std::snprintf(line, sizeof(line),
                "CPlayerInfo dump\n"
                "pool 0x%08X  objects +0x%X  name at +0x0C  width %u\n"
                "score was read at +0x%X, ping at +0x%X, and both came back 0\n"
                "while the in-game scoreboard shows real numbers.\n\n",
                static_cast<unsigned>(layout.player_pool),
                static_cast<unsigned>(layout.object_array),
                static_cast<unsigned>(layout.string_width),
                static_cast<unsigned>(layout.score_at),
                static_cast<unsigned>(layout.ping_at));
  file << line;

  file << "CPlayerPool header - the local row on the scoreboard pins this\n";
  for (std::uint32_t offset = 0; offset < 0x60; offset += 4) {
    std::uint32_t value = 0;
    if (!asi::mem::Read<std::uint32_t>(layout.player_pool + offset, &value))
      break;
    // Both halves are printed because the id is sixteen bits and lands
    // wherever the packing puts it.
    std::snprintf(line, sizeof(line),
                  "  +0x%02X  %08X   u32 %-10u  lo %-6u hi %-6u\n", offset,
                  value, value, value & 0xFFFF, value >> 16);
    file << line;
  }
  file << "\n";

  int dumped = 0;
  for (int id = 0; id < kMaxPlayers && dumped < 12; ++id) {
    if (present[id] == 0) continue;
    const std::uintptr_t info = objects[id];

    std::string name;
    ReadStdString(info + 0x0C, layout.string_variant, &name);
    std::snprintf(line, sizeof(line), "\n--- id %d  \"%s\"  at 0x%08X\n", id,
                  name.c_str(), static_cast<unsigned>(info));
    file << line;
    ++dumped;

    for (std::uint32_t offset = 0; offset < 0x60; offset += 4) {
      std::uint32_t value = 0;
      if (!asi::mem::Read<std::uint32_t>(info + offset, &value)) break;

      std::string note;
      if (value == 0) {
        note = "0";
      } else if (IsHeapPointer(value)) {
        note = "-> heap";
      } else if (value < 100000) {
        note = "int " + std::to_string(value);
      }
      if (offset >= 0x0C && offset < 0x0C + layout.string_width)
        note += "  (inside the name)";

      char text[6] = {};
      for (int b = 0; b < 4; ++b) {
        const unsigned char byte =
            static_cast<unsigned char>((value >> (b * 8)) & 0xFF);
        text[b] = (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
      }
      std::snprintf(line, sizeof(line), "  +0x%02X  %08X  %-28s |%s|\n", offset,
                    value, note.c_str(), text);
      file << line;
    }

    // CPlayerInfo is only the front door. Whatever the scoreboard is drawing
    // may well live in the CRemotePlayer it points at, which nothing here has
    // looked inside yet.
    std::uint32_t remote = 0;
    if (!asi::mem::Read<std::uint32_t>(info, &remote) || !IsHeapPointer(remote))
      continue;
    std::snprintf(line, sizeof(line), "  CRemotePlayer at 0x%08X\n",
                  static_cast<unsigned>(remote));
    file << line;

    for (std::uint32_t offset = 0; offset < 0x80; offset += 4) {
      std::uint32_t value = 0;
      if (!asi::mem::Read<std::uint32_t>(remote + offset, &value)) break;

      std::string note;
      if (value == 0) {
        note = "0";
      } else if (IsHeapPointer(value)) {
        note = "-> heap";
      } else if (value < 100000) {
        note = "int " + std::to_string(value);
      } else {
        const float as_float = *reinterpret_cast<const float*>(&value);
        if (as_float > -20000.0f && as_float < 20000.0f &&
            (as_float > 0.01f || as_float < -0.01f)) {
          char buffer[32];
          std::snprintf(buffer, sizeof(buffer), "float %.2f", as_float);
          note = buffer;
        }
      }
      std::snprintf(line, sizeof(line), "    r+0x%02X  %08X  %s\n", offset,
                    value, note.c_str());
      file << line;
    }
  }

  LOG_INFO("wrote {} - pool resolved but no player has a ping", path);
}

bool LooksLikeVehicleArrays(std::uintptr_t pool, std::uint32_t offset) {
  const std::uintptr_t objects = pool + offset;
  const std::uintptr_t flags   = objects + kMaxVehicles * 4;
  const std::uintptr_t game    = flags + kMaxVehicles * 4;
  if (!asi::mem::IsReadable(objects, kMaxVehicles * 4 * 3)) return false;

  const auto* object_values = reinterpret_cast<const std::uint32_t*>(objects);
  const auto* flag_values   = reinterpret_cast<const std::uint32_t*>(flags);
  const auto* game_values   = reinterpret_cast<const std::uint32_t*>(game);

  // A cheap sample first: two thousand slots is a lot to walk for every
  // candidate offset, and garbage fails on the first few.
  for (int i = 0; i < kMaxVehicles; i += 128)
    if (flag_values[i] > 1) return false;

  auto plausible = [](std::uint32_t value) {
    return value == 0 ||
           (value >= 0x00010000u && value < 0xC0000000u && value % 4 == 0);
  };

  int flagged = 0;
  int with_entity = 0;
  for (int i = 0; i < kMaxVehicles; ++i) {
    // The signature is an array of booleans flanked by two arrays of
    // pointers. Requiring the flag and the wrapper to agree was an assumption
    // too far: the server can know about a vehicle that is not streamed to us,
    // and that is exactly the case this failed on.
    if (flag_values[i] > 1) return false;
    if (!plausible(object_values[i]) || !plausible(game_values[i])) return false;
    if (flag_values[i] != 0) ++flagged;
    if (game_values[i] != 0) ++with_entity;
  }
  if (flagged == 0 || flagged >= kMaxVehicles) return false;
  if (with_entity == 0) return false;

  int checked = 0;
  for (int i = 0; i < kMaxVehicles && checked < 4; ++i) {
    if (game_values[i] == 0) continue;
    if (!IsHeapPointer(game_values[i])) return false;
    ++checked;
  }
  return checked > 0;
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

  // Pools: a block of mostly-heap pointers at the tail of CNetGame.
  int pool_candidates = 0;
  for (std::uint32_t offset = kPoolsSearchFrom; offset < kPoolsSearchTo;
       offset += kSearchStep) {
    std::uint32_t candidate = 0;
    if (!asi::mem::Read<std::uint32_t>(net_game + offset, &candidate)) continue;
    if (!IsHeapPointer(candidate)) continue;
    if (!asi::mem::IsReadable(candidate, kPoolCount * 4)) continue;

    // Requiring all nine pools to be non-null was too strict - the block is
    // recognised by mostly being pointers, and the player pool is then
    // identified by what it contains rather than by its index.
    if (!asi::mem::IsReadable(candidate, kPoolSlotsToTry * 4)) continue;
    const auto* entries = reinterpret_cast<const std::uint32_t*>(candidate);
    int pointer_like = 0;
    for (int i = 0; i < kPoolCount; ++i)
      if (entries[i] != 0 && IsHeapPointer(entries[i])) ++pointer_like;
    if (pointer_like < kPoolCount - 3) continue;
    ++pool_candidates;

    for (int slot = 0; slot < kPoolSlotsToTry && !layout.player_pool; ++slot) {
      const std::uintptr_t player_pool = entries[slot];
      if (player_pool == 0 || !IsHeapPointer(player_pool)) continue;
      for (std::uint32_t inner = 0; inner < kArraySearchTo;
           inner += kSearchStep) {
        if (!LooksLikeSlotArrays(player_pool, inner)) continue;
        layout.pools           = candidate;
        layout.player_pool     = player_pool;
        layout.object_array    = inner;
        layout.not_empty_array = inner + kMaxPlayers * 4;
        break;
      }
    }
    if (layout.player_pool) break;
  }

  if (!layout.player_pool) {
    static bool dumped = false;
    if (!dumped) {
      dumped = true;
      DumpNetGame(net_game, layout.host);
    }
    layout.note = "found CNetGame (host " + layout.host + ") and " +
                  std::to_string(pool_candidates) +
                  " pool-block candidates, but none of them held an array of "
                  "1004 player slots";
    g_layout = layout;
    g_resolved = false;
    return g_layout;
  }

  // Which std::string layout the client was built with is decided against the
  // players in the pool, not against the local player: there are dozens of
  // them to agree with each other, and CPlayerInfo puts the name at a fixed
  // +0x0C whatever the packing, because every member before it is four bytes.
  {
    const auto* objects = reinterpret_cast<const std::uint32_t*>(
        layout.player_pool + layout.object_array);
    const auto* present = reinterpret_cast<const std::uint32_t*>(
        layout.player_pool + layout.not_empty_array);

    for (int variant = 0; variant < 2 && layout.string_variant < 0; ++variant) {
      int agreed = 0;
      int tried  = 0;
      for (int i = 0; i < kMaxPlayers && tried < 6; ++i) {
        if (present[i] == 0) continue;
        ++tried;
        std::string name;
        if (ReadStdString(objects[i] + 0x0C, variant, &name) && !name.empty())
          ++agreed;
      }
      // One lucky read proves nothing; every player agreeing does.
      if (tried > 0 && agreed == tried) layout.string_variant = variant;
    }
  }
  if (layout.string_variant < 0) {
    layout.note = "player pool found, but no player name reads as a "
                  "std::string in either layout";
    g_layout = layout;
    g_resolved = true;
    return g_layout;
  }

  // The local player's own record sits between the largest id and the slot
  // arrays. Its exact offset depends on the packed width of the id, so it is
  // searched for rather than computed - and it is not worth failing over.
  for (std::uint32_t offset = 4; offset < layout.object_array; ++offset) {
    std::string name;
    if (ReadStdString(layout.player_pool + offset, layout.string_variant,
                      &name) &&
        name.size() >= 3) {
      layout.local_name = offset;
      break;
    }
  }

  // CPlayerPool::m_localInfo continues { name, CLocalPlayer*, ping, score }.
  // That pointer is the anchor: only the correct string width puts a heap
  // pointer immediately after the name, so the width is established by fact
  // rather than by which of two guesses looks better.
  if (layout.local_name != 0) {
    for (std::uint32_t width : {24u, 28u}) {
      std::uint32_t object = 0;
      if (!asi::mem::Read<std::uint32_t>(
              layout.player_pool + layout.local_name + width, &object))
        continue;
      if (!IsHeapPointer(object)) continue;
      layout.string_width = width;
      break;
    }
  }

  // The shape search can land a few slots early and still pass: shifting both
  // arrays by the same amount keeps them correlated, and the fields it slides
  // onto are zero here. The local record gives the true base - m_localInfo is
  // { id, align, name, CLocalPlayer*, ping, score }, so the arrays start
  // twelve bytes after the name ends.
  if (layout.local_name != 0 && layout.string_width != 0) {
    const std::uint32_t derived = layout.local_name + layout.string_width + 12;
    if (derived != layout.object_array && LooksLikeSlotArrays(
                                              layout.player_pool, derived)) {
      LOG_INFO("player slots move from +0x{:X} to +0x{:X}, derived from the "
               "local record", layout.object_array, derived);
      layout.object_array    = derived;
      layout.not_empty_array = derived + kMaxPlayers * 4;
    }
  }

  if (layout.string_width != 0) {
    // CPlayerInfo is { CRemotePlayer*, BOOL isNPC, alignment, name, score,
    // ping } - so both offsets follow from the width, with nothing guessed.
    layout.score_at = 0x0C + layout.string_width;
    layout.ping_at  = layout.score_at + 4;

    // Confirmation has to come from the shape of CPlayerInfo, not from any
    // value being non-zero: plenty of servers never send scores or pings at
    // all, and treating that as a failure would condemn a correct read.
    const auto* objects = reinterpret_cast<const std::uint32_t*>(
        layout.player_pool + layout.object_array);
    const auto* present = reinterpret_cast<const std::uint32_t*>(
        layout.player_pool + layout.not_empty_array);

    int checked = 0;
    int well_formed = 0;
    for (int i = 0; i < kMaxPlayers && checked < 16; ++i) {
      if (present[i] == 0) continue;
      ++checked;

      std::uint32_t remote = 0;
      std::uint32_t is_npc = 0;
      asi::mem::Read<std::uint32_t>(objects[i], &remote);
      asi::mem::Read<std::uint32_t>(objects[i] + 0x04, &is_npc);

      // A remote player pointer that is null or a heap address, and an NPC
      // flag that is a flag. Anything else means we are not looking at a
      // CPlayerInfo.
      if (is_npc <= 1 && (remote == 0 || IsHeapPointer(remote))) ++well_formed;
    }
    layout.confirmed = checked >= 4 && well_formed == checked;
  }

  // The local id is the field before the local name, not the first plausible
  // number in the structure - scanning from zero found the largest-id field
  // and reported somebody else's slot as our own.
  //
  // CPlayerPool::m_localInfo is { ID, alignment padding, name }, so the id is
  // six or eight bytes back depending on how wide ID is. Both are checked
  // against the occupancy array, because we are never one of the remote slots.
  if (layout.local_name >= 8) {
    const auto* present = reinterpret_cast<const std::uint32_t*>(
        layout.player_pool + layout.not_empty_array);
    // The declaration puts the id right before the alignment padding and the
    // name, so it is taken from there rather than searched for. Rejecting a
    // candidate because its slot is occupied was a mistake: it assumed the
    // local player is absent from the remote pool, and on this server that
    // assumption pushed the search onto a field of zeroes instead.
    const std::uint32_t offset = layout.local_name - 6;
    std::uint16_t value = 0;
    if (offset >= 4 &&
        asi::mem::Read<std::uint16_t>(layout.player_pool + offset, &value) &&
        value < kMaxPlayers) {
      layout.local_id_at = offset;
      layout.local_id_occupied = present[value] != 0;
    }
  }

  // The vehicle pool lives in the same block of pointers as the player pool.
  if (layout.pools != 0) {
    for (int slot = 0; slot < kPoolSlotsToTry && layout.vehicle_pool == 0;
         ++slot) {
      std::uint32_t candidate = 0;
      if (!asi::mem::Read<std::uint32_t>(layout.pools + slot * 4, &candidate))
        continue;
      if (!IsHeapPointer(candidate)) continue;
      for (std::uint32_t inner = kVehicleSearchFrom; inner < kVehicleSearchTo;
           ++inner) {
        if (!LooksLikeVehicleArrays(candidate, inner)) continue;
        layout.vehicle_pool    = candidate;
        layout.vehicle_objects = inner;
        LOG_INFO("vehicle pool at 0x{:08X}, objects +0x{:X}", candidate, inner);
        break;
      }
    }
    if (layout.vehicle_pool == 0)
      LOG_WARN("no vehicle pool found in the block at 0x{:08X}", layout.pools);
  }

  layout.valid = true;
  layout.note  = "resolved";
  g_layout     = layout;
  g_resolved   = true;

  LOG_INFO("SA-MP layout resolved: CNetGame=0x{:08X} host={} pools=0x{:08X} "
           "playerPool=0x{:08X} objects=+0x{:X} localName=+0x{:X} "
           "string layout {}",
           layout.net_game, layout.host, layout.pools, layout.player_pool,
           layout.object_array, layout.local_name, layout.string_variant);
  return g_layout;
}

bool DumpPlayerRecords() {
  const Layout& layout = ResolveLayout();
  if (!layout.valid) return false;
  DumpPlayerInfo(layout);
  return true;
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

  // Whether anyone has a ping is a property of this moment, not of the
  // layout: the server sends scores and pings periodically, so seconds after
  // joining they are all legitimately zero. Deciding it once, at resolve time,
  // meant a correct read looked broken for the rest of the session.
  std::size_t with_ping = 0;
  std::size_t streamed = 0;
  int largest_id = -1;
  for (int id = 0; id < kMaxPlayers; ++id) {
    if (present[id] == 0) continue;
    largest_id = id;
    const std::uintptr_t info = objects[id];

    // CPlayerInfo: CRemotePlayer*, BOOL is-NPC, alignment, then the name.
    std::uint32_t is_npc = 0;
    std::string   name;
    asi::mem::Read<std::uint32_t>(info + 0x04, &is_npc);
    if (!ReadStdString(info + 0x0C, layout.string_variant, &name)) continue;

    json entry{{"id", id}, {"name", name}, {"npc", is_npc != 0}};

    // Most of a busy server is not streamed in, and saying so is more useful
    // than a position of zero.
    std::uint32_t remote = 0;
    if (asi::mem::Read<std::uint32_t>(info, &remote) && IsHeapPointer(remote)) {
      std::uint8_t team = 0;
      std::uint8_t state = 0;
      if (asi::mem::Read<std::uint8_t>(remote + kRemoteTeam, &team) &&
          team != 255)
        entry["team"] = team;
      if (asi::mem::Read<std::uint8_t>(remote + kRemoteState, &state)) {
        entry["state"] = state;
        // SA-MP's own state, which is what the server told us, rather than the
        // game ped's vehicle field - see below for why that one lies.
        entry["in_vehicle"] = state == kStateDriver || state == kStatePassenger;
      }

      std::uint32_t vehicle = 0;
      if (asi::mem::Read<std::uint32_t>(remote + kRemoteVeh, &vehicle) &&
          IsHeapPointer(vehicle))
        entry["in_vehicle"] = true;

      std::uint32_t samp_ped = 0;
      asi::mem::Read<std::uint32_t>(remote + kRemotePed, &samp_ped);
      const std::uint32_t game_ped = GamePedOfSampPed(samp_ped);
      const Position position = ReadEntityPosition(game_ped);
      entry["streamed"] = position.valid;
      if (position.valid) {
        entry["pos"] = {position.x, position.y, position.z};
        ++streamed;
      }

      // No health, armour or weapon here, and not because they are hard to
      // reach. The game ped of a remote player is a local puppet: SA-MP gives
      // it a large health value so it cannot die on our machine, since damage
      // is the server's to decide - which is why every player read back as
      // 1000 hp holding a fist. What is true is m_fReportedHealth, sent by the
      // server, and that arrives in the sync packets along with score and
      // ping. It belongs to that work, not to this.
    } else {
      entry["streamed"] = false;
    }

    if (layout.ping_at != 0) {
      std::uint32_t ping = 0;
      std::int32_t  score = 0;
      if (asi::mem::Read<std::uint32_t>(info + layout.ping_at, &ping)) {
        entry["ping"] = ping;
        if (ping > 0 && ping < 1500) ++with_ping;
      }
      if (asi::mem::Read<std::int32_t>(info + layout.score_at, &score))
        entry["score"] = score;
    }
    players.push_back(std::move(entry));
  }

  std::string local_name;
  if (layout.local_name != 0)
    ReadStdString(layout.player_pool + layout.local_name, layout.string_variant,
                  &local_name);
  json vehicles = json::array();
  if (layout.vehicle_pool != 0) {
    const std::uintptr_t objects = layout.vehicle_pool + layout.vehicle_objects;
    const std::uintptr_t flags   = objects + kMaxVehicles * 4;
    const std::uintptr_t game    = flags + kMaxVehicles * 4;
    if (asi::mem::IsReadable(objects, kMaxVehicles * 4 * 3)) {
      const auto* flag_values = reinterpret_cast<const std::uint32_t*>(flags);
      const auto* game_values = reinterpret_cast<const std::uint32_t*>(game);

      for (int id = 0; id < kMaxVehicles; ++id) {
        if (flag_values[id] == 0) continue;
        const std::uint32_t entity = game_values[id];
        if (!IsHeapPointer(entity)) continue;

        const Position position = ReadEntityPosition(entity);
        if (!position.valid) continue;

        json entry{{"id", id}, {"pos", {position.x, position.y, position.z}}};
        std::int16_t model = 0;
        // Model ids are reported as the game stores them; naming two hundred
        // of them from memory is exactly the sort of guess to avoid.
        if (asi::mem::Read<std::int16_t>(entity + kEntityModel, &model) &&
            model > 0)
          entry["model"] = model;
        vehicles.push_back(std::move(entry));
      }
    }
  }

  json self{{"name", local_name}};
  if (layout.string_width != 0) {
    const std::uintptr_t after =
        layout.player_pool + layout.local_name + layout.string_width;
    std::uint32_t ping = 0;
    std::int32_t  score = 0;
    if (asi::mem::Read<std::uint32_t>(after + 4, &ping)) self["ping"] = ping;
    if (asi::mem::Read<std::int32_t>(after + 8, &score)) self["score"] = score;
  }
  // CPlayerPool::m_localInfo holds a CLocalPlayer*, and that starts with the
  // same CPed wrapper the remote players use.
  if (layout.string_width != 0) {
    std::uint32_t local_player = 0;
    if (asi::mem::Read<std::uint32_t>(
            layout.player_pool + layout.local_name + layout.string_width,
            &local_player) &&
        IsHeapPointer(local_player)) {
      std::uint32_t samp_ped = 0;
      asi::mem::Read<std::uint32_t>(local_player, &samp_ped);
      const std::uint32_t game_ped = GamePedOfSampPed(samp_ped);
      const Position position = ReadEntityPosition(game_ped);
      if (position.valid) {
        self["pos"] = {position.x, position.y, position.z};
        self.update(ReadPedDetails(game_ped, /*full=*/true));
      }
    }
  }

  if (layout.local_id_at != 0) {
    std::uint16_t local_id = 0;
    if (asi::mem::Read<std::uint16_t>(layout.player_pool + layout.local_id_at,
                                      &local_id)) {
      self["id"] = local_id;
      // If that id is also in the remote pool, the name held there settles
      // whether it is really us.
      if (present[local_id] != 0) {
        std::string pool_name;
        if (ReadStdString(objects[local_id] + 0x0C, layout.string_variant,
                          &pool_name))
          self["name_in_pool"] = pool_name;
      }
    }
  }

  // Counted before the move: reading size() off a container that has already
  // been moved out of is how this reported zero next to six hundred players.
  const std::size_t player_count = players.size();
  return json{
      {"resolved", true},
      {"host", layout.host},
      {"largest_id", largest_id},
      // Enough to tell a wrong offset from a server that simply reports zero.
      {"layout",
       {{"confirmed", layout.confirmed},
        {"string_width", layout.string_width},
        {"local_name_at", layout.local_name},
        {"local_id_at", layout.local_id_at},
        {"score_at", layout.score_at},
        {"ping_at", layout.ping_at},
        {"ping_populated", with_ping > 0},
        {"players_with_ping", with_ping},
        {"players_streamed", streamed},
        {"local_id_occupied", layout.local_id_occupied}}},
      {"self", std::move(self)},
      {"player_count", player_count},
      {"players", std::move(players)},
      {"vehicle_count", vehicles.size()},
      {"vehicles", std::move(vehicles)},
  };
}

}  // namespace gtabot::samp
