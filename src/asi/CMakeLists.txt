add_library(bot MODULE
  dllmain.cpp
  types.cpp
  log.cpp
  crash_log.cpp
  bridge.cpp
  hooks/frame.cpp
  samp/version.cpp
  samp/discovery.cpp
  state/memory.cpp
  state/probe.cpp
  mcp/server.cpp
  mcp/rpc.cpp
  mcp/http.cpp
  mcp/tools.cpp
  ui/overlay.cpp
  ui/status_source.cpp)

target_include_directories(bot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bot PRIVATE
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  minhook
  imgui
  psapi
  ws2_32)
target_compile_definitions(bot PRIVATE
  GTABOT_VERSION="${PROJECT_VERSION}"
  SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE)

# The ASI loader scans for *.asi, so the extension is not optional.
set_target_properties(bot PROPERTIES
  OUTPUT_NAME "bot"
  SUFFIX ".asi")
