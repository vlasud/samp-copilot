add_library(bot MODULE
  dllmain.cpp
  types.cpp
  log.cpp
  crash_log.cpp
  bridge.cpp
  actions/experiments.cpp
  actions/travel.cpp
  actions/walker.cpp
  game/exe.cpp
  game/world_query.cpp
  game/input_probe.cpp
  game/pad_watch.cpp
  game/map_marker.cpp
  game/mouse_watch.cpp
  game/watchpoint.cpp
  game/collision.cpp
  game/api_trace.cpp
  game/paths.cpp
  nav/planner.cpp
  hooks/cursor.cpp
  hooks/windowmode.cpp
  hooks/frame.cpp
  samp/version.cpp
  samp/discovery.cpp
  samp/chat.cpp
  samp/world.cpp
  samp/input_state.cpp
  state/memory.cpp
  state/probe.cpp
  mcp/server.cpp
  mcp/rpc.cpp
  mcp/http.cpp
  mcp/tools.cpp
  ui/overlay.cpp
  ui/status_source.cpp)

target_include_directories(bot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# The panel speaks Russian, and its strings live in the source as UTF-8.
target_compile_options(bot PRIVATE /utf-8)
target_link_libraries(bot PRIVATE
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  minhook
  imgui
  psapi
  winmm
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
