include(FetchContent)

# CMake 4 dropped compatibility with `cmake_minimum_required(VERSION <3.5)`,
# which several upstream projects still declare. This is the documented escape
# hatch and applies only to the fetched subprojects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    ON)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
# The .asi resolves its log path from the game directory, which may contain
# characters that do not survive a narrow encoding. Keep filenames wide.
set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.14.1
  GIT_SHALLOW    ON)

FetchContent_MakeAvailable(nlohmann_json spdlog)
