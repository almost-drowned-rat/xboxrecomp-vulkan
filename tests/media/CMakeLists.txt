cmake_minimum_required(VERSION 3.20)
project(media_test C)
set(CMAKE_C_STANDARD 11)
# Scan rules, decoder output and playlist modes of src/media: no guest, no Vulkan.
add_subdirectory(../../src/media media)
add_executable(media_test test_main.c)
target_link_libraries(media_test PRIVATE xbox_media)
enable_testing()
add_test(NAME media_test COMMAND media_test ${CMAKE_CURRENT_BINARY_DIR}/scratch)
