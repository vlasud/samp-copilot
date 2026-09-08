#include "game/watchpoint.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>

#include "game/api_trace.hpp"
#include "game/exe.hpp"
#include "log.hpp"
#include "samp/version.hpp"
#include "state/memory.hpp"

namespace gtabot::game {
namespace {

// Two-byte read/write traps. The key table entry for W in the copy the
// game reads (CPad::NewKeyState.standardKeys['W']) and the pad's keyboard
// temp state's forward axis (CPad::PCTempKeyState.LeftStickY): anything
// that looks at what the walk holds, other than the game and this module,
// is named here.
constexpr std::uint32_t kNewKeyW    = 0xB73190 + 0x18 + 'W' * 2;   // 0xB73256
constexpr std::uint32_t kTempStickY = 0xB73458 + 0x78 + 0x02;      // 0xB734D2
constexpr int kMaxLogged = 40;

std::atomic<bool> g_handler_installed{false};
std::atomic<std::uintptr_t> g_watch0{0};
std::atomic<std::uintptr_t> g_watch1{0};
std::atomic<unsigned long long> g_hits0{0}, g_hits1{0};
std::atomic<int> g_logged{0};
// Each distinct writing instruction is logged a few times; the flood from
// one site (SetCursorMode every frame) must not drown the one write that
// matters.
constexpr int kWriters = 32;
constexpr int kPerWriter = 3;
constexpr int kTotal = 300;
std::atomic<std::uintptr_t> g_writer_eip[kWriters];
std::atomic<int>            g_writer_hits[kWriters];

bool WorthLogging(std::uintptr_t eip) {
  for (int i = 0; i < kWriters; ++i) {
    std::uintptr_t have = g_writer_eip[i].load();
    if (have == 0) {
      std::uintptr_t expected = 0;
      if (!g_writer_eip[i].compare_exchange_strong(expected, eip)) {
        if (expected != eip) continue;
      }
      have = eip;
    }
    if (have != eip) continue;
    return g_writer_hits[i].fetch_add(1) < kPerWriter;
  }
  return false;   // more distinct writers than slots: the first thirty-two are the story
}
std::set<DWORD> g_threads_done;
unsigned long long g_last_sweep_ms = 0;

std::string Describe(std::uintptr_t address) {
  char buffer[MAX_PATH + 32];
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      module == nullptr) {
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(address));
    return buffer;
  }
  char path[MAX_PATH] = "";
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* name = std::strrchr(path, '\\');
  name = name ? name + 1 : path;
  std::snprintf(buffer, sizeof(buffer), "%s+0x%X", name,
                static_cast<unsigned>(address - reinterpret_cast<std::uintptr_t>(module)));
  return buffer;
}

// The return addresses on the stack, as far as they can be told apart from
// data: dwords that point into some module's code.
std::string StackWalk(std::uintptr_t esp) {
  std::string out;
  int shown = 0;
  for (int i = 0; i < 96 && shown < 8; ++i) {
    std::uint32_t value = 0;
    if (!asi::mem::Read<std::uint32_t>(esp + i * 4, &value)) break;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(static_cast<std::uintptr_t>(value)),
                            &module) ||
        module == nullptr)
      continue;
    // A return address follows a call: the bytes before it should be one.
    std::uint8_t before[5] = {};
    if (asi::mem::ReadGuarded(value - 5, before, 5) != 5) continue;
    const bool call = before[0] == 0xE8 || before[3] == 0xFF || before[2] == 0xFF;
    if (!call) continue;
    if (!out.empty()) out += " < ";
    out += Describe(value);
    ++shown;
  }
  return out.empty() ? "(no return addresses recognised)" : out;
}

// gta_sa.exe and this module: the expected traffic, not logged.
bool KnownAccessor(std::uintptr_t eip) {
  static HMODULE exe = GetModuleHandleA(nullptr);
  static HMODULE self = nullptr;
  if (self == nullptr)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&KnownAccessor), &self);
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(eip), &module))
    return false;
  return module == exe || module == self;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  if (info == nullptr || info->ExceptionRecord == nullptr ||
      info->ContextRecord == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
    return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* ctx = info->ContextRecord;
  const DWORD dr6 = ctx->Dr6;
  if ((dr6 & 0x3) == 0) return EXCEPTION_CONTINUE_SEARCH;   // not our watch
  const int which = (dr6 & 0x1) ? 0 : 1;
  const std::uintptr_t address = which == 0 ? g_watch0.load() : g_watch1.load();
  (which == 0 ? g_hits0 : g_hits1).fetch_add(1);
  ctx->Dr6 = 0;
  // Resume flag, so the instruction is not trapped again. (Data breakpoints
  // trap after the access, so this is only about single-step semantics.)
  ctx->EFlags |= 0x10000;

  // The game reading its own table and this module holding the keys are
  // the traffic; everything else is the finding.
  if (!KnownAccessor(ctx->Eip) && WorthLogging(ctx->Eip) &&
      g_logged.fetch_add(1) < kTotal) {
    std::uint16_t now_value = 0;
    asi::mem::Read<std::uint16_t>(address, &now_value);
    LOG_ERROR("watchpoint: {} read or written by {} on thread {} - value now {}; "
              "stack: {}",
              which == 0 ? "the key table's W (CPad::NewKeyState, 0xB73256)"
                         : "the pad's keyboard temp forward axis (0xB734D2)",
              Describe(ctx->Eip), GetCurrentThreadId(), now_value,
              StackWalk(ctx->Esp));
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

bool ArmThread(DWORD tid, std::uintptr_t a0, std::uintptr_t a1) {
  HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                 THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                             FALSE, tid);
  if (thread == nullptr) return false;
  bool ok = false;
  if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(thread, &ctx)) {
      ctx.Dr0 = a0;
      ctx.Dr1 = a1;
      ctx.Dr6 = 0;
      // L0, L1 enabled; RW0 = write (01), LEN0 = 4 bytes (11); same for 1.
      DWORD dr7 = ctx.Dr7;
      dr7 |= 0x1 | 0x4;
      // Read or write (RW = 11), two bytes (LEN = 01), for both.
      dr7 &= ~(0xF << 16);
      dr7 |= (0x3 << 16) | (0x1 << 18);
      dr7 &= ~(0xF << 20);
      dr7 |= (0x3 << 20) | (0x1 << 22);
      ctx.Dr7 = dr7;
      ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      ok = SetThreadContext(thread, &ctx) != FALSE;
    }
    ResumeThread(thread);
  }
  CloseHandle(thread);
  return ok;
}

// The calling thread cannot set its own debug registers through
// SetThreadContext reliably; a helper thread does it for it.
struct ArmSelf { DWORD tid; std::uintptr_t a0, a1; bool ok; };
DWORD WINAPI ArmSelfThread(LPVOID param) {
  auto* job = static_cast<ArmSelf*>(param);
  job->ok = ArmThread(job->tid, job->a0, job->a1);
  return 0;
}

}  // namespace

void WatchpointsInstall() {
  // Two-byte watches on two-byte boundaries.
  const std::uintptr_t a0 = At(kNewKeyW);
  const std::uintptr_t a1 = At(kTempStickY);
  if (a0 == 0 || a1 == 0) return;
  g_watch0.store(a0);
  g_watch1.store(a1);

  if (!g_handler_installed.exchange(true)) {
    AddVectoredExceptionHandler(1, &Handler);
    LOG_INFO("watchpoints: read/write traps on the key table's W (0x{:08X}) and the "
             "pad's keyboard temp forward axis (0x{:08X}); every thread gets them",
             static_cast<unsigned>(a0), static_cast<unsigned>(a1));
  }

  const unsigned long long now = GetTickCount64();
  if (now - g_last_sweep_ms < 2000) return;
  g_last_sweep_ms = now;

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  int armed = 0, failed = 0;
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID != pid) continue;
      if (g_threads_done.count(entry.th32ThreadID)) continue;
      bool ok = false;
      if (entry.th32ThreadID == self) {
        ArmSelf job{self, a0, a1, false};
        HANDLE helper = CreateThread(nullptr, 0, &ArmSelfThread, &job, 0, nullptr);
        if (helper) {
          WaitForSingleObject(helper, 2000);
          CloseHandle(helper);
        }
        ok = job.ok;
      } else {
        ok = ArmThread(entry.th32ThreadID, a0, a1);
      }
      g_threads_done.insert(entry.th32ThreadID);
      if (ok) ++armed; else ++failed;
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  if (armed > 0 || failed > 0)
    LOG_INFO("watchpoints: armed on {} thread(s){}", armed,
             failed > 0 ? " (" + std::to_string(failed) + " could not be)" : "");
}

std::string WatchpointLine() {
  char text[48];
  std::snprintf(text, sizeof(text), " wp=%llu/%llu",
                static_cast<unsigned long long>(g_hits0.load()),
                static_cast<unsigned long long>(g_hits1.load()));
  return text;
}

}  // namespace gtabot::game
