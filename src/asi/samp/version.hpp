#pragma once
//
// Identifies which SA-MP client we were injected next to.
//
// Every structure offset in the client is version-specific, so nothing that
// reads SA-MP memory may run until this reports a known build. Detection uses
// the loaded module's PE headers (SizeOfImage + TimeDateStamp) rather than
// hashing the file: it is a handful of reads, needs no disk access, and cannot
// be fooled by a same-sized file swapped in after load.
//
#include <cstdint>
#include <string>

namespace gtabot::samp {

enum class Version {
  kUnknown = 0,
  k037R1,
  k037R2,
  k037R3,
  k037R4,
  k037R5,
  k037DL,
};

struct Client {
  Version       version = Version::kUnknown;
  // Base address of samp.dll; 0 when the module is not loaded yet.
  std::uintptr_t base = 0;
  std::uint32_t size_of_image = 0;
  std::uint32_t timestamp     = 0;
};

const char* ToString(Version v);

// Returns a Client with `base` set once samp.dll is present. `version` may
// still be kUnknown if the fingerprint is not in the table.
Client Detect();

// Blocks until samp.dll is loaded or `timeout_ms` elapses. The ASI is loaded
// well before SA-MP initialises, so callers must wait rather than assume.
Client WaitForClient(unsigned timeout_ms);

}  // namespace gtabot::samp
