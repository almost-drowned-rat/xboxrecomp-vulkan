cmake_minimum_required(VERSION 3.20)
project(host_wait_test C)
set(CMAKE_C_STANDARD 11)
# Only the platform layer: no runtime.
add_subdirectory(../../src/platform platform)
add_executable(host_wait_test test_main.c)
target_link_libraries(host_wait_test PRIVATE platform)
enable_testing()
add_test(NAME host_wait_test COMMAND host_wait_test)
