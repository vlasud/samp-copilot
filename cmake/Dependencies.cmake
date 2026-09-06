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

# Inline x86 hooking. Small, battle-tested, no runtime dependencies.
set(MINHOOK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.4
  GIT_SHALLOW    ON)

# The in-game debug overlay. Pinned to the last of the 1.91 line: 1.92 reworked
# the font and texture API, and the DX9 backend here is the well-worn one.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.9b
  GIT_SHALLOW    ON)

FetchContent_MakeAvailable(nlohmann_json spdlog minhook imgui)

# Dear ImGui ships no build system, so the sources and the two backends we need
# are compiled into a target of our own.
add_library(imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp")

target_include_directories(imgui PUBLIC
  "${imgui_SOURCE_DIR}"
  "${imgui_SOURCE_DIR}/backends")

# Drawing happens inside the game's own render call, where there is no room for
# an unwind, and the overlay never allocates from a failing path anyway.
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS)
target_link_libraries(imgui PUBLIC d3d9 dwmapi)
