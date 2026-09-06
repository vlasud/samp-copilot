#include "ui/status_source.hpp"

#include <mutex>

namespace gtabot::asi {
namespace {

std::mutex          g_mutex;
StatusSource::Mcp   g_mcp;

}  // namespace

void StatusSource::SetMcp(Mcp status) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_mcp = std::move(status);
}

StatusSource::Mcp StatusSource::mcp() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_mcp;
}

}  // namespace gtabot::asi
