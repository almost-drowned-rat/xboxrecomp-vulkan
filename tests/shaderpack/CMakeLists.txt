cmake_minimum_required(VERSION 3.20)
project(shaderpack_test C)
set(CMAKE_C_STANDARD 11)
# Zip reader, zip-slip guard, pack extraction and the preset index of
# src/shaderpack: no guest, no Vulkan; SDL3 for files (CMAKE_PREFIX_PATH).
add_subdirectory(../../src/shaderpack shaderpack)
add_executable(shaderpack_test test_main.c)
target_link_libraries(shaderpack_test PRIVATE xbox_shaderpack)
enable_testing()
add_test(NAME shaderpack_test COMMAND shaderpack_test ${CMAKE_CURRENT_BINARY_DIR}/scratch)
