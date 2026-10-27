cmake_minimum_required(VERSION 3.20)
project(input_bind_test C)
set(CMAKE_C_STANDARD 11)
# Only the binding logic of src/input (xinput_bind.c): no SDL.
add_executable(input_bind_test test_main.c ../../src/input/xinput_bind.c)
target_include_directories(input_bind_test PRIVATE ../../src/input ../../src)
enable_testing()
add_test(NAME input_bind_test COMMAND input_bind_test)
