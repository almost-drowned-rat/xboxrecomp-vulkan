cmake_minimum_required(VERSION 3.20)
project(range_alloc_test C)
set(CMAKE_C_STANDARD 11)
# Only the range bookkeeping of nv2a_vk's texture memory: no Vulkan.
add_executable(range_alloc_test test_main.c ../../src/nv2a_vk/range_alloc.c)
target_include_directories(range_alloc_test PRIVATE ../../src/nv2a_vk)
enable_testing()
add_test(NAME range_alloc_test COMMAND range_alloc_test)
