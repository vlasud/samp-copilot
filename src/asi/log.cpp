#include "log.hpp"

#include <windows.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <deque>
#include <memory>
#include <mutex>

namespace gtabot::asi {
namespace {

constexpr std::size_t kRingCapacity = 300;

std::mutex          g_ring_mutex;
std::deque<LogLine> g_ring;

// Mirrors every record into the ring the in-game overlay draws from, so the
// tail is visible without alt-tabbing out to read the file.
class RingSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    std::string text = fmt::to_string(formatted);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
      text.pop_back();

    std::lock_guard<std::mutex> lock(g_ring_mutex);
    if (g_ring.size() >= kRingCapacity) g_ring.pop_front();
    g_ring.push_back(LogLine{static_cast<int>(msg.level), std::move(text)});
  }
  void flush_() override {}
};

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
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        full, /*truncate=*/true));
    sinks.push_back(std::make_shared<RingSink>());

    auto logger =
        std::make_shared<spdlog::logger>("asi", sinks.begin(), sinks.end());
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%t] %v");
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
  } catch (const std::exception&) {
    // A read-only game folder must not take the mod down with it.
  }
}

void ShutdownLogging() { spdlog::shutdown(); }

std::vector<LogLine> RecentLogLines(std::size_t max_lines) {
  std::lock_guard<std::mutex> lock(g_ring_mutex);
  const std::size_t take = max_lines < g_ring.size() ? max_lines : g_ring.size();
  return std::vector<LogLine>(g_ring.end() - static_cast<std::ptrdiff_t>(take),
                              g_ring.end());
}

}  // namespace gtabot::asi
