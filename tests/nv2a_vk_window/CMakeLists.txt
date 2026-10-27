cmake_minimum_required(VERSION 3.20)
project(nv2a_vk_window C CXX)

# nv2a_vk_window - the Vulkan back end on its own: open its window, clear a
# 640x480 surface to a different colour each frame and present it. No title,
# no kernel; the few executor symbols nv2a_vk reads are stubbed in
# test_main.c. Needs glslang, SDL3 and Vulkan (CMAKE_PREFIX_PATH).
#
#   nv2a-vk-window [frames]     exit 0 if every frame was presented
#   RECOMP_GL_DUMP=<prefix>,<n> also writes every n-th frame as BMP

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/nv2a_vk
                 ${CMAKE_CURRENT_BINARY_DIR}/nv2a_vk)

add_executable(nv2a-vk-window test_main.c)
target_link_libraries(nv2a-vk-window PRIVATE xbox_nv2a_vk)
if(MINGW)
    # One file to run: C++ runtime and winpthread linked in.
    target_link_options(nv2a-vk-window PRIVATE -static)
endif()
