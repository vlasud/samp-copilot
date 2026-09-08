#include "game/threads.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gtabot::game {
namespace {

std::string ModuleAt(std::uintptr_t address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(address), &module) ||
      module == nullptr)
    return "-";
  char path[MAX_PATH] = {};
  if (GetModuleFileNameA(module, path, MAX_PATH) == 0) return "-";
  const char* name = std::strrchr(path, 0x5C);   // the path separator
  name = name ? name + 1 : path;
  char text[128];
  std::snprintf(text, sizeof(text), "%s+0x%X", name,
                static_cast<unsigned>(address -
                                      reinterpret_cast<std::uintptr_t>(module)));
  return text;
}

}  // namespace

std::string WhereThreadsAre() {
  const DWORD me = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return "the thread list is not readable";
  std::string out;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID != me) continue;
      if (entry.th32ThreadID == self) continue;
      const HANDLE thread =
          OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                     entry.th32ThreadID);
      if (thread == nullptr) continue;
      char line[256];
      if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(thread, &context)) {
          std::snprintf(line, sizeof(line), "%lu at %s; ",
                        static_cast<unsigned long>(entry.th32ThreadID),
                        ModuleAt(context.Eip).c_str());
          out += line;
        }
        ResumeThread(thread);
      }
      CloseHandle(thread);
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return out.empty() ? "no other threads" : out;
}

}  // namespace gtabot::game
