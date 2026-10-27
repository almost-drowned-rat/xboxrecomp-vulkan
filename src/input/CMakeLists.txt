# Xbox Input: host pads (SDL3, every OS), keyboard and pad script

add_library(xbox_input STATIC
    xinput_bind.c
    xinput_device.c
    xinput_script.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# SDL3 on every host (A10: one code path; replaces Windows XInput and SDL2).
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(xbox_input PUBLIC platform SDL3::SDL3)

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
