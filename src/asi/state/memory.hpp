#pragma once
//
// Read-only access to the game process, with every read validated first.
//
// A wrong offset must produce a logged miss, never a crash: this module is
// loaded into someone's running game, and an access violation here is their
// session gone. Nothing in here writes to memory.
//
#include <cstdint>
#include <string>
#include <vector>

namespace gtabot::asi::mem {

struct Module {
  std::uintptr_t base = 0;
  std::uint32_t  size = 0;
  bool valid() const { return base != 0; }
  bool contains(std::uintptr_t address) const {
    return address >= base && address < base + size;
  }
};

struct Region {
  std::uintptr_t base = 0;
  std::size_t    size = 0;
  bool           is_private = false;
  bool           is_writable = false;
};

Module FindModule(const wchar_t* name);

// "d3d9.dll+0x1B70", or "0x680C05D0 (no module)". Used to say whose code we
// are about to hook, and where a crash actually happened.
std::string DescribeAddress(std::uintptr_t address);

// True when [address, address+size) is committed and readable.
bool IsReadable(std::uintptr_t address, std::size_t size);

// Reads a POD only if the whole object is readable. Returns false otherwise,
// leaving `out` untouched.
template <typename T>
bool Read(std::uintptr_t address, T* out) {
  if (!IsReadable(address, sizeof(T))) return false;
  *out = *reinterpret_cast<const T*>(address);
  return true;
}

// Reads a NUL-terminated ASCII string, stopping at `max_length` or at the
// first byte outside the printable range. Empty on any failure.
std::string ReadCString(std::uintptr_t address, std::size_t max_length);

// Every committed readable region of the process, or of one module when
// `within` is given.
std::vector<Region> ReadableRegions(const Module* within = nullptr);

struct Hit {
  std::uintptr_t address = 0;
  // Offset from the module the search was scoped to, when it was scoped.
  std::uint32_t  rva = 0;
};

// Searches for a literal byte sequence. `budget_bytes` caps the work so an
// exhaustive scan cannot stall the caller indefinitely.
std::vector<Hit> Scan(const std::vector<Region>& regions,
                      const std::string& needle, std::size_t max_hits,
                      std::size_t budget_bytes, std::uintptr_t rva_origin = 0);

// Finds 4-byte values inside `regions` that look like pointers into committed
// private memory. This is how an unknown build's root pointer gets located
// without guessing an offset.
std::vector<Hit> ScanPointerCandidates(const std::vector<Region>& regions,
                                       std::size_t max_hits,
                                       std::uintptr_t rva_origin = 0);

}  // namespace gtabot::asi::mem
