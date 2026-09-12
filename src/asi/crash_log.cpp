#include "crash_log.hpp"

#include <windows.h>

#include <MinHook.h>

#include <cstdio>
#include <intrin.h>
#include <string>

#include "log.hpp"
#include "state/memory.hpp"

namespace gtabot::asi {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

const char* CodeName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    default:                              return "exception";
  }
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    const auto address = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);

    LOG_ERROR("CRASH {} (0x{:08X}) at {} on thread {}", CodeName(record.ExceptionCode),
              static_cast<unsigned int>(record.ExceptionCode),
              mem::DescribeAddress(address),
              static_cast<unsigned int>(GetCurrentThreadId()));

    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record.NumberParameters >= 2) {
      const char* how = record.ExceptionInformation[0] == 0   ? "reading"
                        : record.ExceptionInformation[0] == 1 ? "writing"
                                                              : "executing";
      LOG_ERROR("       while {} 0x{:08X}", how,
                static_cast<unsigned int>(record.ExceptionInformation[1]));
    }

    // Whose code was on the stack matters more than the exact frame: this says
    // straight away whether it was ours.
    if (info->ContextRecord) {
      LOG_ERROR("       eip={} esp=0x{:08X}",
                mem::DescribeAddress(info->ContextRecord->Eip),
                static_cast<unsigned int>(info->ContextRecord->Esp));
    }
    spdlog::default_logger()->flush();
  }

  // Hand back to whatever was installed before us. The game and the launcher
  // have their own crash handling and it is not ours to swallow.
  if (g_previous) return g_previous(info);
  return EXCEPTION_CONTINUE_SEARCH;
}

using ExitProcessFn = void(WINAPI*)(UINT);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);

ExitProcessFn      g_real_exit = nullptr;
TerminateProcessFn g_real_terminate = nullptr;

// What the window looked like when the request came in. The sessions that end
// this way have been ending seconds after the focus moved, so whether this
// window still had it is the first thing worth knowing.
std::string WindowState() {
  const HWND foreground = GetForegroundWindow();
  char cls[64] = "";
  if (foreground) GetClassNameA(foreground, cls, sizeof(cls));
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "foreground=%s%s",
                foreground ? cls : "none",
                foreground == GetActiveWindow() ? " (ours)" : "");
  return buffer;
}

void WINAPI HookedExitProcess(UINT code) {
  LOG_ERROR("the session is ending from inside: ExitProcess({}) asked for by {} - {}",
            static_cast<unsigned>(code),
            mem::DescribeAddress(reinterpret_cast<std::uintptr_t>(_ReturnAddress())),
            WindowState());
  spdlog::default_logger()->flush();
  g_real_exit(code);
}

BOOL WINAPI HookedTerminateProcess(HANDLE process, UINT code) {
  DWORD target = 0;
  if (process == GetCurrentProcess()) target = GetCurrentProcessId();
  else target = GetProcessId(process);
  if (target == GetCurrentProcessId()) {
    LOG_ERROR("the session is being killed from inside: TerminateProcess({}) asked for "
              "by {} - {}", static_cast<unsigned>(code),
              mem::DescribeAddress(reinterpret_cast<std::uintptr_t>(_ReturnAddress())),
              WindowState());
    spdlog::default_logger()->flush();
  }
  return g_real_terminate(process, code);
}

}  // namespace

void InstallCrashLogger() {
  g_previous = SetUnhandledExceptionFilter(&OnUnhandled);
  LOG_INFO("crash logger installed");
}

void WatchProcessExit() {
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (kernel32 == nullptr) {
    LOG_WARN("kernel32 is not loaded - the end of the session will go unexplained");
    return;
  }
  auto* exit_process = reinterpret_cast<void*>(GetProcAddress(kernel32, "ExitProcess"));
  auto* terminate = reinterpret_cast<void*>(GetProcAddress(kernel32, "TerminateProcess"));
  bool watched = false;
  if (exit_process != nullptr &&
      MH_CreateHook(exit_process, &HookedExitProcess,
                    reinterpret_cast<void**>(&g_real_exit)) == MH_OK &&
      MH_EnableHook(exit_process) == MH_OK)
    watched = true;
  if (terminate != nullptr &&
      MH_CreateHook(terminate, &HookedTerminateProcess,
                    reinterpret_cast<void**>(&g_real_terminate)) == MH_OK &&
      MH_EnableHook(terminate) == MH_OK)
    watched = true;
  LOG_INFO("exit watch {}: a session that ends with no crash and no exit line was "
           "killed from outside this process", watched ? "installed" : "FAILED");
}

}  // namespace gtabot::asi
