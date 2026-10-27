# XDK library HLE layer: XDK functions at API level, bound per title by address
# (overrides in the title's recomp_manual.c). Reference design: XboxXBE-Recomp
# runtime/xdk (MIT). The LLE paths (src/usb, src/apu, the pushbuffer executor)
# stay the fallback.

add_library(xbox_xdk STATIC
    xdk_xinput.c
    xdk_dsound.c
    xdk_soundtrack.c
)

target_include_directories(xbox_xdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_xdk PUBLIC xbox_kernel xbox_input xbox_media)
# xbox_adpcm.h only (header): the HLE does not link the APU emulation.
target_include_directories(xbox_xdk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../apu)
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(xbox_xdk PUBLIC SDL3::SDL3)
if(NOT WIN32)
    target_link_libraries(xbox_xdk PUBLIC m)
endif()

if(MSVC)
    target_compile_options(xbox_xdk PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_xdk PRIVATE -Wall -Wextra -O2)
endif()
