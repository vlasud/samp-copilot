#pragma once
//
// Identifies the game executable, and turns its published addresses into
// addresses in this process.
//
// Everything in game/ calls into gta_sa.exe rather than reading it, and a
// call to the wrong place is not a wrong answer, it is the session gone. So
// nothing here is callable until the executable has been recognised: the
// only build these addresses are for is 1.0 US, and it is identified the way
// samp.dll is, by the link timestamp in its PE header. That is the same value
// Windows prints in a crash report, which is how it was read off this very
// machine.
//
#include <cstdint>

namespace gtabot::game {

struct Exe {
  std::uintptr_t base           = 0;
  std::uint32_t  timestamp      = 0;
  std::uint32_t  size_of_image  = 0;
  // Whether this is the build the addresses below are for.
  bool           known          = false;
};

const Exe& Detect();

// The address in this process of a 1.0 US virtual address, or 0 when the
// build is not the one those addresses belong to. Callers must treat 0 as
// "do not call", not as an error to retry.
std::uintptr_t At(std::uint32_t virtual_address);

}  // namespace gtabot::game
