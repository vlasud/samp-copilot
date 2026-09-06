#include "version.hpp"

#include <windows.h>

namespace gtabot::samp {
namespace {

struct Fingerprint {
  std::uint32_t size_of_image;
  std::uint32_t timestamp;
  Version       version;
};

// Only entries marked "verified" were measured against a real file. The rest
// must be filled in the same way before the matching client is trusted:
//   python tools/fingerprint_samp.py <path to samp.dll>
constexpr Fingerprint kKnown[] = {
    // verified: D:\SAMP\samp.dll, 2 199 552 bytes, 2015-05-01
    {0x00330000u, 0x5542F47Au, Version::k037R1},
};

bool ReadHeaders(std::uintptr_t base, std::uint32_t* size_of_image,
                 std::uint32_t* timestamp) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  *size_of_image = nt->OptionalHeader.SizeOfImage;
  *timestamp     = nt->FileHeader.TimeDateStamp;
  return true;
}

}  // namespace

const char* ToString(Version v) {
  switch (v) {
    case Version::k037R1: return "0.3.7-R1";
    case Version::k037R2: return "0.3.7-R2";
    case Version::k037R3: return "0.3.7-R3";
    case Version::k037R4: return "0.3.7-R4";
    case Version::k037R5: return "0.3.7-R5";
    case Version::k037DL: return "0.3.DL";
    default:              return "unknown";
  }
}

Client Detect() {
  Client c;
  HMODULE module = GetModuleHandleW(L"samp.dll");
  if (!module) return c;

  c.base = reinterpret_cast<std::uintptr_t>(module);
  if (!ReadHeaders(c.base, &c.size_of_image, &c.timestamp)) {
    c.base = 0;
    return c;
  }
  for (const Fingerprint& fp : kKnown) {
    if (fp.size_of_image == c.size_of_image && fp.timestamp == c.timestamp) {
      c.version = fp.version;
      break;
    }
  }
  return c;
}

Client WaitForClient(unsigned timeout_ms) {
  constexpr unsigned kStepMs = 100;
  for (unsigned waited = 0;; waited += kStepMs) {
    Client c = Detect();
    if (c.base) return c;
    if (waited >= timeout_ms) return c;
    Sleep(kStepMs);
  }
}

}  // namespace gtabot::samp
