cmake_minimum_required(VERSION 3.20)
project(game_setup_test C)
set(CMAKE_C_STANDARD 11)
# Only the installer: no runtime, no SDL.
add_subdirectory(../../src/install install)
add_executable(game_setup_test test_main.c)
target_link_libraries(game_setup_test PRIVATE xbox_install)
enable_testing()
add_test(NAME game_setup_test COMMAND game_setup_test ${CMAKE_CURRENT_BINARY_DIR}/layout)
