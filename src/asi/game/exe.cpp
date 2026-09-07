#include "game/exe.hpp"

#include <windows.h>

#include "log.hpp"
#include "state/memory.hpp"

namespace gtabot::game {
namespace {

// gta_sa.exe 1.0 US links at 0x400000 and, on this machine, carries this
// timestamp - read off the crash report Windows wrote when a bug of ours took
// the game down, which is as direct a reading as one gets.
constexpr std::uintptr_t kPreferredBase = 0x400000;
constexpr std::uint32_t  kTimestamp10US = 0x427101CA;

Exe  g_exe;
bool g_detected = false;

}  // namespace

const Exe& Detect() {
  if (g_detected) return g_exe;

  const asi::mem::Module module = asi::mem::FindModule(nullptr);
  if (!module.valid()) return g_exe;

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
  if (!asi::mem::IsReadable(module.base, sizeof(IMAGE_DOS_HEADER)) ||
      dos->e_magic != IMAGE_DOS_SIGNATURE)
    return g_exe;
  const std::uintptr_t nt_at = module.base + static_cast<std::uint32_t>(dos->e_lfanew);
  if (!asi::mem::IsReadable(nt_at, sizeof(IMAGE_NT_HEADERS32))) return g_exe;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(nt_at);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return g_exe;

  g_exe.base          = module.base;
  g_exe.timestamp     = nt->FileHeader.TimeDateStamp;
  g_exe.size_of_image = nt->OptionalHeader.SizeOfImage;
  g_exe.known         = g_exe.timestamp == kTimestamp10US;
  g_detected          = true;

  if (g_exe.known)
    LOG_INFO("gta_sa.exe is 1.0 US (timestamp 0x{:08X}) at 0x{:08X} - game "
             "calls enabled", g_exe.timestamp, g_exe.base);
  else
    LOG_WARN("gta_sa.exe timestamp 0x{:08X} is not the 1.0 US build - "
             "nothing will call into the game", g_exe.timestamp);
  return g_exe;
}

std::uintptr_t At(std::uint32_t virtual_address) {
  const Exe& exe = Detect();
  if (!exe.known) return 0;
  return exe.base + (virtual_address - kPreferredBase);
}

}  // namespace gtabot::game
