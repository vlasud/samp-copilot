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

}  // namespace

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

    const auto* begin = reinterpret_cast<const char*>(region.base);
    const auto* end   = begin + region.size - needle.size() + 1;
    for (const char* p = begin; p < end; ++p) {
      if (std::memcmp(p, needle.data(), needle.size()) != 0) continue;
      Hit hit;
      hit.address = reinterpret_cast<std::uintptr_t>(p);
      hit.rva = rva_origin ? static_cast<std::uint32_t>(hit.address - rva_origin)
                           : 0;
      hits.push_back(hit);
      if (hits.size() >= max_hits) break;
    }
    scanned += region.size;
  }
  return hits;
}

std::vector<Hit> ScanPointerCandidates(const std::vector<Region>& regions,
                                       std::size_t max_hits,
                                       std::uintptr_t rva_origin) {
  std::vector<Hit> hits;
  for (const Region& region : regions) {
    if (hits.size() >= max_hits) break;
    if (!region.is_writable) continue;  // a root pointer lives in writable data

    const auto* values = reinterpret_cast<const std::uint32_t*>(region.base);
    const std::size_t count = region.size / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < count; ++i) {
      const std::uintptr_t value = values[i];
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
      if (hits.size() >= max_hits) break;
    }
  }
  return hits;
}

}  // namespace gtabot::asi::mem
