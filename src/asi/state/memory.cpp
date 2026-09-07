#include "state/memory.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gtabot::asi::mem {
namespace {

// User-mode address space of a 32-bit process. Large-address-aware builds go
// to 0xC0000000, and VirtualQuery simply reports nothing above whatever the
// process actually has.
constexpr std::uintptr_t kUserSpaceEnd = 0xC0000000u;

// How much of a region a sweep copies out at a time before looking at it.
constexpr std::size_t kSweepWords = 2048;  // 8 KB

bool IsReadableProtection(DWORD protect) {
  if (protect & PAGE_GUARD) return false;
  if (protect & PAGE_NOACCESS) return false;
  return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool IsWritableProtection(DWORD protect) {
  return (protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Scans one region for a byte sequence, tolerating the region going away
// mid-scan. VirtualQuery only says a page was mapped a moment ago; another
// thread is free to unmap it while we read, and a diagnostic must not be able
// to fault for that. No C++ objects here, which is what __try requires.
std::size_t ScanRegionGuarded(const char* begin, std::size_t bytes,
                              const char* needle, std::size_t needle_length,
                              std::uintptr_t* hits, std::size_t max_hits) {
  std::size_t found = 0;
  __try {
    if (bytes < needle_length) return 0;
    const char* end = begin + bytes - needle_length + 1;
    for (const char* p = begin; p < end && found < max_hits; ++p) {
      if (std::memcmp(p, needle, needle_length) != 0) continue;
      hits[found++] = reinterpret_cast<std::uintptr_t>(p);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Whatever was found before the page disappeared is still valid.
  }
  return found;
}

// A page at a time, so a fault costs only the page it happened on and
// everything before it still comes back.
std::size_t CopyGuarded(const void* from, void* to, std::size_t bytes) {
  constexpr std::size_t kPage = 4096;
  std::size_t done = 0;
  __try {
    while (done < bytes) {
      std::size_t step = bytes - done;
      if (step > kPage) step = kPage;
      std::memcpy(static_cast<char*>(to) + done,
                  static_cast<const char*>(from) + done, step);
      done += step;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Whatever arrived before the page went away is still good.
  }
  return done;
}

}  // namespace

std::size_t ReadGuarded(std::uintptr_t address, void* out, std::size_t bytes) {
  if (address == 0 || bytes == 0) return 0;
  return CopyGuarded(reinterpret_cast<const void*>(address), out, bytes);
}

Module FindModule(const wchar_t* name) {
  Module module;
  HMODULE handle = GetModuleHandleW(name);
  if (!handle) return module;

  MODULEINFO info{};
  if (!GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info)))
    return module;

  module.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
  module.size = info.SizeOfImage;
  return module;
}

std::string DescribeAddress(std::uintptr_t address) {
  char buffer[64];
  HMODULE owner = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &owner) ||
      !owner) {
    std::snprintf(buffer, sizeof(buffer), "0x%08X (no module)",
                  static_cast<unsigned int>(address));
    return buffer;
  }

  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(owner, path, MAX_PATH);
  std::wstring wide(path);
  const std::size_t slash = wide.find_last_of(L"\\/");
  if (slash != std::wstring::npos) wide = wide.substr(slash + 1);

  // Narrowing wchar_t to char by truncation loses anything outside ASCII, and
  // a module path is not guaranteed to stay inside it.
  std::string name;
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()), nullptr,
                                         0, nullptr, nullptr);
  if (needed > 0) {
    name.resize(static_cast<std::size_t>(needed));
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        name.data(), needed, nullptr, nullptr);
  }
  std::snprintf(buffer, sizeof(buffer), "+0x%X",
                static_cast<unsigned int>(address -
                                          reinterpret_cast<std::uintptr_t>(owner)));
  return name + buffer;
}

bool IsReadable(std::uintptr_t address, std::size_t size) {
  if (address == 0 || size == 0) return false;
  if (address > kUserSpaceEnd - size) return false;

  std::uintptr_t cursor = address;
  const std::uintptr_t end = address + size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0)
      return false;
    if (mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect))
      return false;
    cursor = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}

std::string ReadCString(std::uintptr_t address, std::size_t max_length) {
  std::string out;
  out.reserve(max_length);
  for (std::size_t i = 0; i < max_length; ++i) {
    char c = 0;
    if (!Read<char>(address + i, &c)) return {};
    if (c == 0) break;
    if (static_cast<unsigned char>(c) < 0x20) return {};
    out.push_back(c);
  }
  return out;
}

std::vector<Region> ReadableRegions(const Module* within) {
  std::vector<Region> regions;
  std::uintptr_t cursor = within ? within->base : 0x10000u;
  const std::uintptr_t end =
      within ? within->base + within->size : kUserSpaceEnd;

  while (cursor < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0)
      break;
    const std::uintptr_t region_base =
        reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);

    if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect)) {
      Region region;
      region.base = std::max(region_base, cursor);
      // MEMORY_BASIC_INFORMATION::RegionSize is ULONG_PTR, which is a distinct
      // type from uintptr_t on 32-bit MSVC, so the comparison type is pinned.
      region.size = static_cast<std::size_t>(
          std::min<std::uintptr_t>(region_base + mbi.RegionSize, end) -
          region.base);
      region.is_private  = mbi.Type == MEM_PRIVATE;
      region.is_writable = IsWritableProtection(mbi.Protect);
      if (region.size > 0) regions.push_back(region);
    }
    cursor = region_base + mbi.RegionSize;
  }
  return regions;
}

std::vector<Hit> Scan(const std::vector<Region>& regions,
                      const std::string& needle, std::size_t max_hits,
                      std::size_t budget_bytes, std::uintptr_t rva_origin) {
  std::vector<Hit> hits;
  if (needle.empty()) return hits;

  std::size_t scanned = 0;
  for (const Region& region : regions) {
    if (hits.size() >= max_hits || scanned >= budget_bytes) break;
    if (region.size < needle.size()) continue;

    std::uintptr_t addresses[64];
    constexpr std::size_t kSlots = sizeof(addresses) / sizeof(addresses[0]);
    const std::size_t room = max_hits - hits.size();
    const std::size_t found = ScanRegionGuarded(
        reinterpret_cast<const char*>(region.base), region.size, needle.data(),
        needle.size(), addresses, room < kSlots ? room : kSlots);
    for (std::size_t i = 0; i < found; ++i) {
      Hit hit;
      hit.address = addresses[i];
      hit.rva = rva_origin ? static_cast<std::uint32_t>(hit.address - rva_origin)
                           : 0;
      hits.push_back(hit);
    }
    scanned += region.size;
  }
  return hits;
}

std::vector<Hit> ScanPointerCandidates(const std::vector<Region>& regions,
                                       std::size_t max_hits,
                                       std::uintptr_t rva_origin) {
  std::vector<Hit> hits;
  std::vector<std::uint32_t> chunk(kSweepWords);
  for (const Region& region : regions) {
    if (hits.size() >= max_hits) break;
    if (!region.is_writable) continue;  // a root pointer lives in writable data

    const std::size_t count = region.size / sizeof(std::uint32_t);
    std::size_t have = 0;   // words of this region currently in `chunk`
    std::size_t from = 0;   // index of chunk[0] within the region
    for (std::size_t i = 0; i < count; ++i) {
      if (i >= from + have) {
        from = i;
        const std::size_t want =
            std::min(kSweepWords, count - from) * sizeof(std::uint32_t);
        have = ReadGuarded(region.base + from * sizeof(std::uint32_t),
                           chunk.data(), want) /
               sizeof(std::uint32_t);
        if (have == 0) break;  // the region went away under us
      }
      const std::uintptr_t value = chunk[i - from];
      // A heap object pointer: aligned, plausible, and pointing at memory the
      // process actually allocated for itself.
      if (value < 0x00100000u || value >= kUserSpaceEnd) continue;
      if (value % 4 != 0) continue;

      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery(reinterpret_cast<LPCVOID>(value), &mbi, sizeof(mbi)) == 0)
        continue;
      if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) continue;
      if (!IsWritableProtection(mbi.Protect)) continue;

      Hit hit;
      hit.address = region.base + i * sizeof(std::uint32_t);
      hit.rva = rva_origin ? static_cast<std::uint32_t>(hit.address - rva_origin)
                           : 0;
      hits.push_back(hit);
      if (hits.size() >= max_hits) break;  // NOLINT
    }
  }
  return hits;
}

}  // namespace gtabot::asi::mem
