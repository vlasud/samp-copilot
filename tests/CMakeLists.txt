# The parts that can be checked without the game.
#
# Most of this project only means anything inside gta_sa.exe: an address, a
# pad, a pool. Some of it is not like that - reading a chat line, deciding
# what a row of a menu says, adding up what is known about a stranger - and
# those are the parts that were being written and shipped on the strength of
# one look at the screen. They run here instead, against text taken from the
# real server.
add_executable(bot_tests
  test_talk.cpp
  test_people.cpp
  test_field.cpp
  main.cpp
  "${CMAKE_SOURCE_DIR}/src/asi/samp/talk.cpp"
  "${CMAKE_SOURCE_DIR}/src/asi/state/people.cpp"
  "${CMAKE_SOURCE_DIR}/src/asi/nav/grid.cpp"
)
target_include_directories(bot_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/asi")
target_link_libraries(bot_tests PRIVATE nlohmann_json::nlohmann_json)
# The sources are UTF-8 and full of Russian, which MSVC will otherwise read in
# the machine's code page and mangle.
if(MSVC)
  target_compile_options(bot_tests PRIVATE /utf-8)
endif()

add_test(NAME bot_tests COMMAND bot_tests)
