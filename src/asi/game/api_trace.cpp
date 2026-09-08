#include "game/api_trace.hpp"

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>

#include "log.hpp"
#include "samp/version.hpp"

namespace gtabot::game {
namespace {

constexpr int kRing = 8192;
constexpr unsigned long long kDumpWindowMs = 2000;
constexpr int kDumpTail = 120;
// samp.dll's protected region, where the decisions are made.
constexpr std::uintptr_t kProtectedFrom = 0x220000;
constexpr std::uintptr_t kProtectedTo   = 0x310000;

struct Entry {
  unsigned long long ms;
  const char*        api;
  std::uintptr_t     a1, a2;
  std::uintptr_t     caller;   // offset inside samp.dll
  char               text[24];
};

Entry g_ring[kRing];
std::atomic<unsigned> g_head{0};
std::atomic<unsigned long long> g_total{0};
std::atomic<bool> g_installed{false};
std::atomic<std::uintptr_t> g_samp_base{0};
std::atomic<std::uintptr_t> g_samp_end{0};
std::atomic<unsigned long long> g_dump_asked_ms{0};
char g_dump_why[96] = "";
// Reentrancy guard per thread: our own logging calls these very functions.
thread_local bool t_inside = false;

bool FromSamp(std::uintptr_t ret, std::uintptr_t* offset) {
  const std::uintptr_t base = g_samp_base.load(std::memory_order_relaxed);
  const std::uintptr_t end  = g_samp_end.load(std::memory_order_relaxed);
  if (base == 0 || ret < base || ret >= end) return false;
  *offset = ret - base;
  return true;
}

void Record(const char* api, std::uintptr_t a1, std::uintptr_t a2, void* ret,
            const char* text = nullptr) {
  std::uintptr_t offset = 0;
  if (!FromSamp(reinterpret_cast<std::uintptr_t>(ret), &offset)) return;
  const unsigned slot = g_head.fetch_add(1, std::memory_order_relaxed) % kRing;
  Entry& e = g_ring[slot];
  e.ms = GetTickCount64();
  e.api = api;
  e.a1 = a1;
  e.a2 = a2;
  e.caller = offset;
  e.text[0] = 0;
  if (text != nullptr) {
    // A string argument, if it is one: a class name, a module, an export.
    __try {
      std::strncpy(e.text, text, sizeof(e.text) - 1);
      e.text[sizeof(e.text) - 1] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      e.text[0] = 0;
    }
  }
  g_total.fetch_add(1, std::memory_order_relaxed);
}

#define TRACE(NAME, RET, PARAMS, ARGS, A1, A2, TEXT)                         \
  RET(WINAPI* orig_##NAME) PARAMS = nullptr;                                 \
  RET WINAPI hook_##NAME PARAMS {                                            \
    if (!t_inside) {                                                         \
      t_inside = true;                                                       \
      Record(#NAME, static_cast<std::uintptr_t>(A1),                         \
             static_cast<std::uintptr_t>(A2), _ReturnAddress(), TEXT);       \
      t_inside = false;                                                      \
    }                                                                        \
    return orig_##NAME ARGS;                                                 \
  }

// user32
TRACE(GetAsyncKeyState, SHORT, (int vk), (vk), vk, 0, nullptr)
TRACE(GetKeyState, SHORT, (int vk), (vk), vk, 0, nullptr)
TRACE(GetKeyboardState, BOOL, (PBYTE state), (state), 0, 0, nullptr)
TRACE(FindWindowA, HWND, (LPCSTR cls, LPCSTR title), (cls, title), 0, 0, cls ? cls : title)
TRACE(EnumWindows, BOOL, (WNDENUMPROC fn, LPARAM lp), (fn, lp), (std::uintptr_t)fn, 0, nullptr)
TRACE(GetForegroundWindow, HWND, (), (), 0, 0, nullptr)
TRACE(GetActiveWindow, HWND, (), (), 0, 0, nullptr)
TRACE(GetWindowTextA, int, (HWND w, LPSTR s, int n), (w, s, n), (std::uintptr_t)w, n, nullptr)
TRACE(GetClassNameA, int, (HWND w, LPSTR s, int n), (w, s, n), (std::uintptr_t)w, n, nullptr)
TRACE(GetWindowThreadProcessId, DWORD, (HWND w, LPDWORD pid), (w, pid), (std::uintptr_t)w, 0, nullptr)
TRACE(SetWindowsHookExA, HHOOK, (int id, HOOKPROC fn, HINSTANCE m, DWORD tid), (id, fn, m, tid), id, tid, nullptr)
TRACE(ClipCursor, BOOL, (const RECT* r), (r), (std::uintptr_t)r, 0, nullptr)
TRACE(ShowCursor, int, (BOOL show), (show), show, 0, nullptr)
TRACE(GetGUIThreadInfo, BOOL, (DWORD tid, PGUITHREADINFO info), (tid, info), tid, 0, nullptr)
TRACE(GetWindowLongA, LONG, (HWND w, int idx), (w, idx), (std::uintptr_t)w, idx, nullptr)
// kernel32
TRACE(GetTickCount, DWORD, (), (), 0, 0, nullptr)
TRACE(QueryPerformanceCounter, BOOL, (LARGE_INTEGER* c), (c), 0, 0, nullptr)
TRACE(VirtualQuery, SIZE_T, (LPCVOID at, PMEMORY_BASIC_INFORMATION mbi, SIZE_T n), (at, mbi, n), (std::uintptr_t)at, n, nullptr)
TRACE(VirtualProtect, BOOL, (LPVOID at, SIZE_T n, DWORD prot, PDWORD old), (at, n, prot, old), (std::uintptr_t)at, n, nullptr)
TRACE(ReadProcessMemory, BOOL, (HANDLE h, LPCVOID at, LPVOID out, SIZE_T n, SIZE_T* got), (h, at, out, n, got), (std::uintptr_t)at, n, nullptr)
TRACE(WriteProcessMemory, BOOL, (HANDLE h, LPVOID at, LPCVOID in, SIZE_T n, SIZE_T* put), (h, at, in, n, put), (std::uintptr_t)at, n, nullptr)
TRACE(GetModuleHandleA, HMODULE, (LPCSTR name), (name), 0, 0, name)
TRACE(GetModuleHandleW, HMODULE, (LPCWSTR name), (name), (std::uintptr_t)name, 0, nullptr)
TRACE(GetProcAddress, FARPROC, (HMODULE m, LPCSTR name), (m, name), (std::uintptr_t)m, 0, (reinterpret_cast<std::uintptr_t>(name) > 0xFFFF ? name : nullptr))
TRACE(LoadLibraryA, HMODULE, (LPCSTR name), (name), 0, 0, name)
TRACE(CreateToolhelp32Snapshot, HANDLE, (DWORD flags, DWORD pid), (flags, pid), flags, pid, nullptr)
TRACE(GetThreadContext, BOOL, (HANDLE t, LPCONTEXT c), (t, c), (std::uintptr_t)t, c ? c->ContextFlags : 0, nullptr)
TRACE(OpenThread, HANDLE, (DWORD access, BOOL inherit, DWORD tid), (access, inherit, tid), access, tid, nullptr)
TRACE(OpenProcess, HANDLE, (DWORD access, BOOL inherit, DWORD pid), (access, inherit, pid), access, pid, nullptr)
TRACE(IsDebuggerPresent, BOOL, (), (), 0, 0, nullptr)
TRACE(CheckRemoteDebuggerPresent, BOOL, (HANDLE p, PBOOL out), (p, out), (std::uintptr_t)p, 0, nullptr)
TRACE(K32EnumProcessModules, BOOL, (HANDLE p, HMODULE* mods, DWORD cb, LPDWORD needed), (p, mods, cb, needed), cb, 0, nullptr)
TRACE(GetModuleFileNameA, DWORD, (HMODULE m, LPSTR s, DWORD n), (m, s, n), (std::uintptr_t)m, n, nullptr)
TRACE(CreateThread, HANDLE, (LPSECURITY_ATTRIBUTES sa, SIZE_T st, LPTHREAD_START_ROUTINE fn, LPVOID p, DWORD f, LPDWORD id), (sa, st, fn, p, f, id), (std::uintptr_t)fn, 0, nullptr)
// winmm
TRACE(timeGetTime, DWORD, (), (), 0, 0, nullptr)
// ntdll
TRACE(NtQueryInformationProcess, LONG, (HANDLE p, int cls, PVOID out, ULONG n, PULONG got), (p, cls, out, n, got), cls, n, nullptr)
TRACE(NtQuerySystemInformation, LONG, (int cls, PVOID out, ULONG n, PULONG got), (cls, out, n, got), cls, n, nullptr)

#undef TRACE

struct Target {
  const wchar_t* module;
  const char*    name;
  void*          hook;
  void**         original;
};

#define T(MOD, NAME) {L##MOD, #NAME, reinterpret_cast<void*>(&hook_##NAME), reinterpret_cast<void**>(&orig_##NAME)}
const Target kTargets[] = {
    T("user32", GetAsyncKeyState), T("user32", GetKeyState), T("user32", GetKeyboardState),
    T("user32", FindWindowA), T("user32", EnumWindows), T("user32", GetForegroundWindow),
    T("user32", GetActiveWindow), T("user32", GetWindowTextA), T("user32", GetClassNameA),
    T("user32", GetWindowThreadProcessId), T("user32", SetWindowsHookExA),
    T("user32", ClipCursor), T("user32", ShowCursor), T("user32", GetGUIThreadInfo),
    T("user32", GetWindowLongA),
    T("kernel32", GetTickCount), T("kernel32", QueryPerformanceCounter),
    T("kernel32", VirtualQuery), T("kernel32", VirtualProtect),
    T("kernel32", ReadProcessMemory), T("kernel32", WriteProcessMemory),
    T("kernel32", GetModuleHandleA), T("kernel32", GetModuleHandleW),
    T("kernel32", GetProcAddress), T("kernel32", LoadLibraryA),
    T("kernel32", CreateToolhelp32Snapshot), T("kernel32", GetThreadContext),
    T("kernel32", OpenThread), T("kernel32", OpenProcess), T("kernel32", IsDebuggerPresent),
    T("kernel32", CheckRemoteDebuggerPresent), T("kernel32", K32EnumProcessModules),
    T("kernel32", GetModuleFileNameA), T("kernel32", CreateThread),
    T("winmm", timeGetTime),
    T("ntdll", NtQueryInformationProcess), T("ntdll", NtQuerySystemInformation),
};
#undef T

}  // namespace

bool ApiTraceInstall() {
  if (g_installed.load()) return true;
  const samp::Client client = samp::Detect();
  if (client.base == 0) return false;
  g_samp_base.store(client.base);
  g_samp_end.store(client.base + (client.size_of_image ? client.size_of_image : 0x330000));

  int hooked = 0, failed = 0;
  for (const Target& t : kTargets) {
    const MH_STATUS s = MH_CreateHookApi(t.module, t.name, t.hook, t.original);
    if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
      HMODULE m = GetModuleHandleW(t.module);
      void* fn = m ? reinterpret_cast<void*>(GetProcAddress(m, t.name)) : nullptr;
      if (fn && MH_EnableHook(fn) == MH_OK) ++hooked; else ++failed;
    } else {
      ++failed;
    }
  }
  g_installed.store(true);
  LOG_INFO("api trace: {} Windows functions hooked ({} not), recording calls made "
           "from samp.dll (base 0x{:08X})", hooked, failed,
           static_cast<unsigned>(client.base));
  return true;
}

void ApiTraceRequestDump(const char* why) {
  unsigned long long expected = 0;
  if (g_dump_asked_ms.compare_exchange_strong(expected, GetTickCount64())) {
    std::strncpy(g_dump_why, why ? why : "", sizeof(g_dump_why) - 1);
    g_dump_why[sizeof(g_dump_why) - 1] = 0;
  }
}

void ApiTraceDumpIfAsked() {
  const unsigned long long asked = g_dump_asked_ms.load();
  if (asked == 0) return;
  // Wait a moment so the frame that decided has finished writing.
  if (GetTickCount64() - asked < 300) return;
  g_dump_asked_ms.store(0);

  const unsigned head = g_head.load();
  const unsigned count = head < static_cast<unsigned>(kRing) ? head : static_cast<unsigned>(kRing);
  const unsigned long long from = asked - kDumpWindowMs;
  const unsigned long long until = asked + 30;

  // A copy, oldest first.
  static Entry copy[kRing];
  unsigned n = 0;
  for (unsigned i = 0; i < count; ++i) {
    const Entry& e = g_ring[(head - count + i) % kRing];
    if (e.ms < from || e.ms > until || e.api == nullptr) continue;
    copy[n++] = e;
  }

  std::map<std::string, int> per_api_protected, per_api_plain;
  for (unsigned i = 0; i < n; ++i) {
    const bool prot = copy[i].caller >= kProtectedFrom && copy[i].caller < kProtectedTo;
    (prot ? per_api_protected : per_api_plain)[copy[i].api] += 1;
  }
  std::string summary;
  for (const auto& [api, c] : per_api_protected)
    summary += (summary.empty() ? "" : ", ") + api + "=" + std::to_string(c);
  std::string plain;
  for (const auto& [api, c] : per_api_plain)
    plain += (plain.empty() ? "" : ", ") + api + "=" + std::to_string(c);
  LOG_ERROR("api trace ({}): in the 2 s before, samp.dll's protected region called: "
            "{}; the rest of samp.dll called: {}", g_dump_why,
            summary.empty() ? "nothing hooked" : summary,
            plain.empty() ? "nothing hooked" : plain);

  // The entries right around the write: the last ones before it and the
  // first ones after, not whatever the buffer filled with since.
  unsigned at_write = n;
  for (unsigned i = 0; i < n; ++i)
    if (copy[i].ms >= asked) { at_write = i; break; }
  const unsigned start = at_write > static_cast<unsigned>(kDumpTail) ? at_write - kDumpTail : 0;
  if (at_write + 30 < n) n = at_write + 30;
  std::string lines;
  for (unsigned i = start; i < n; ++i) {
    const Entry& e = copy[i];
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%+lld %s(0x%X,0x%X%s%s)@%s+0x%X",
                  static_cast<long long>(e.ms) - static_cast<long long>(asked), e.api,
                  static_cast<unsigned>(e.a1), static_cast<unsigned>(e.a2),
                  e.text[0] ? "," : "", e.text,
                  (e.caller >= kProtectedFrom && e.caller < kProtectedTo) ? "P" : "s",
                  static_cast<unsigned>(e.caller));
    lines += buf;
    lines += (i + 1 < n) ? " | " : "";
    if (lines.size() > 1800) {
      LOG_ERROR("api trace: {}", lines);
      lines.clear();
    }
  }
  if (!lines.empty()) LOG_ERROR("api trace: {}", lines);
  LOG_ERROR("api trace: end ({} entries in the window; P = protected region, "
            "s = the rest of samp.dll; times relative to the gate write)", n);
}

std::string ApiTraceLine() {
  char text[32];
  std::snprintf(text, sizeof(text), " api=%llu",
                static_cast<unsigned long long>(g_total.load()));
  return text;
}

}  // namespace gtabot::game
