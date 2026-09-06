add_library(gtabot_common STATIC
  protocol.cpp
  pipe.cpp)

target_include_directories(gtabot_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(gtabot_common PUBLIC nlohmann_json::nlohmann_json)
target_compile_definitions(gtabot_common PUBLIC
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE)
