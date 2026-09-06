#pragma once
//
// Logging for the in-game module. The log file lives next to the .asi (i.e. in
// the game folder) because that is the only path we can count on.
//
#include <spdlog/spdlog.h>

#include <string>

namespace gtabot::asi {

// `filename` is resolved relative to this module's own directory.
void InitLogging(const std::string& filename);
void ShutdownLogging();

}  // namespace gtabot::asi

#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
