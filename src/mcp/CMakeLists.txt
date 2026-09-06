add_executable(gta_mcp
  main.cpp
  server.cpp
  state.cpp)

target_include_directories(gta_mcp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gta_mcp PRIVATE gtabot_common)
target_compile_definitions(gta_mcp PRIVATE GTABOT_VERSION="${PROJECT_VERSION}")
set_target_properties(gta_mcp PROPERTIES OUTPUT_NAME "gta-mcp")
