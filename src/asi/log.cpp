#include "log.hpp"

#include <windows.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <memory>

namespace gtabot::asi {
namespace {

HMODULE SelfModule() {
  HMODULE self = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&SelfModule), &self);
  return self;
}

std::wstring SelfDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(SelfModule(), path, MAX_PATH);
  std::wstring s(path, n);
  const std::size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring{} : s.substr(0, slash + 1);
}

}  // namespace

void InitLogging(const std::string& filename) {
  // The caller passes a plain ASCII name; the directory it lands in is the
  // part that can contain anything, so the join happens in wide characters and
  // spdlog is built with SPDLOG_WCHAR_FILENAMES to take it unchanged.
  std::wstring full = SelfDirectory();
  full.append(filename.begin(), filename.end());

  try {
    auto sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(full, /*truncate=*/true);
    auto logger = std::make_shared<spdlog::logger>("asi", std::move(sink));
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v");
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
  } catch (const std::exception&) {
    // A read-only game folder must not take the mod down with it.
  }
}

void ShutdownLogging() { spdlog::shutdown(); }

}  // namespace gtabot::asi
