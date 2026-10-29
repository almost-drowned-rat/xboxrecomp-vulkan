cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#
#   host_wait.*    - host clock + waits for every title (high-resolution
#                    timer, precise deadline wait), both hosts
#   host_info.*    - the log's "version:" and "host:" lines (release name,
#                    game + toolkit commits, Windows / Wine / Steam Deck)
#
# On Windows the real <windows.h> is used, so the library carries host_wait
# and the src/ include path. On Linux it also carries the POSIX primitive
# implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# src/, the directory this one lives in. Resolved relative to here rather than
# via CMAKE_SOURCE_DIR, which points at the *consuming* project's root when a
# game project pulls xboxrecomp in with add_subdirectory() -- the documented
# integration pattern -- and would send "platform/xbox_winnt.h" looking in the
# game's own src/ tree. PROJECT_SOURCE_DIR is no good either: this file
# declares its own project().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(WIN32)
    add_library(platform STATIC host_wait.c host_info.c)
    target_include_directories(platform PUBLIC ${XBOXRECOMP_SRC_DIR})
else()
    add_library(platform STATIC win32_compat.c host_wait.c host_info.c)
    target_include_directories(platform PUBLIC
        ${XBOXRECOMP_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    find_package(Threads REQUIRED)
    target_link_libraries(platform PUBLIC Threads::Threads)
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# host_info.c's commits: regenerated at every build (the header changes only
# after a commit). CMAKE_SOURCE_DIR = the consuming game's repo when a port
# pulls the toolkit in with add_subdirectory(), the toolkit itself otherwise.
set(XR_BUILD_INFO_H "${CMAKE_CURRENT_BINARY_DIR}/generated/xr_build_info.h")
add_custom_target(xr_build_info
    COMMAND ${CMAKE_COMMAND} -DGAME_DIR=${CMAKE_SOURCE_DIR} -DTOOLKIT_DIR=${XBOXRECOMP_SRC_DIR}/..
            -DOUT=${XR_BUILD_INFO_H} -P ${XBOXRECOMP_SRC_DIR}/../cmake/build_info.cmake
    BYPRODUCTS ${XR_BUILD_INFO_H}
    COMMENT "build info (game / toolkit commits)"
    VERBATIM)
add_dependencies(platform xr_build_info)
target_include_directories(platform PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
