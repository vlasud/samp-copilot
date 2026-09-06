#include "crash_log.hpp"

#include <windows.h>

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

}  // namespace

void InstallCrashLogger() {
  g_previous = SetUnhandledExceptionFilter(&OnUnhandled);
  LOG_INFO("crash logger installed");
}

}  // namespace gtabot::asi
