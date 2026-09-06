add_library(bot MODULE
  dllmain.cpp
  log.cpp
  samp/version.cpp)

target_include_directories(bot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bot PRIVATE gtabot_common spdlog::spdlog)
target_compile_definitions(bot PRIVATE
  GTABOT_VERSION="${PROJECT_VERSION}"
  SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

# The ASI loader scans for *.asi, so the extension is not optional.
set_target_properties(bot PROPERTIES
  OUTPUT_NAME "bot"
  SUFFIX ".asi")
