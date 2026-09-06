#pragma once
//
// Logging for the in-game module. The log file lives next to the .asi (i.e. in
// the game folder) because that is the only path we can count on.
//
// Every line is also kept in a small ring buffer so the in-game overlay can
// show the tail without reading the file back.
//
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace gtabot::asi {

// `filename` is resolved relative to this module's own directory.
void InitLogging(const std::string& filename);
void ShutdownLogging();

struct LogLine {
  int         level = 0;  // spdlog::level::level_enum
  std::string text;
};

// Newest last. Safe from any thread.
std::vector<LogLine> RecentLogLines(std::size_t max_lines);

}  // namespace gtabot::asi

#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
