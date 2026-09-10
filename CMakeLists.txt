cmake_minimum_required(VERSION 3.25)

project(gtabot LANGUAGES CXX VERSION 0.1.0)

# gta_sa.exe is a 32-bit process, so everything that gets loaded into it must be
# x86. The MCP server is built x86 as well so the whole project shares one
# toolchain, one preset and one runtime. CMAKE_SIZEOF_VOID_P only exists once a
# compiler has been probed, so this check has to sit after project().
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "This project must be configured for x86, but the toolchain produced a "
    "${CMAKE_SIZEOF_VOID_P}-byte pointer target. Use:  cmake --preset x86")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the .asi is dropped into the game folder and must not depend on a
# redistributable for this exact toolset being installed there.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/Dependencies.cmake)

add_subdirectory(src/asi)

# The parts that can be checked off the game: text, and what is made of it.
enable_testing()
add_subdirectory(tools/fieldcase)
add_subdirectory(tests)
