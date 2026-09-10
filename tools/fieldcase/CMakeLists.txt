# Running a saved field again, without the game.
#
# The mod writes down the grid it was working from whenever a plan ends
# badly; this puts that back to the clearance and the search with the rules
# in the build now. Links nothing of the game - nav/grid.cpp and
# nav/casefile.cpp are written not to know about it - which is the whole
# point: a directory of the places that went wrong runs in seconds.
add_executable(fieldcase
  main.cpp
  "${CMAKE_SOURCE_DIR}/src/asi/nav/grid.cpp"
  "${CMAKE_SOURCE_DIR}/src/asi/nav/casefile.cpp"
)
target_include_directories(fieldcase PRIVATE "${CMAKE_SOURCE_DIR}/src/asi")
if(MSVC)
  target_compile_options(fieldcase PRIVATE /utf-8)
endif()
