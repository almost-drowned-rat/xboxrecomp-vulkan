# The optional boot animation (boot_video.c): media/boot.mpg (MPEG-1 video +
# MP2 audio) decoded on the CPU with pl_mpeg (vendored in third_party/pl_mpeg,
# MIT; see NOTICE and docs/toolkit.md), audio through SDL3, frames handed to
# the renderer through a sink (nv2a_vk_boot_frame). Host code, no guest
# knowledge, no Vulkan.

add_library(xbox_bootvideo STATIC
    boot_video.c
    pl_mpeg_impl.c
)
target_include_directories(xbox_bootvideo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(xbox_bootvideo PUBLIC SDL3::SDL3)

if(MSVC)
    target_compile_options(xbox_bootvideo PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_bootvideo PRIVATE -Wall -Wextra -O2)
    set_source_files_properties(pl_mpeg_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()
