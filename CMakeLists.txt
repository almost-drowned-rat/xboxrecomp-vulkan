cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.9.0 LANGUAGES C)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (147 imports → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Option to build examples

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
add_subdirectory(src/platform)

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

# The D3D11 parts: xbox_d3d8 (D3D8 -> D3D11 for titles that call D3D through
# the kernel), xbox_nv2a's D3D11 PGRAPH and the Media Foundation/D3D8 movie
# player in xbox_video. A title whose D3D is lifted code feeding the
# pushbuffer executor, rendered by nv2a_vk, needs none of them:
# XBOXRECOMP_D3D11=OFF builds a Vulkan-only runtime without d3d11/dxgi.
option(XBOXRECOMP_D3D11 "Build the D3D11 libraries (xbox_d3d8, xbox_nv2a, the D3D8 movie player)" ON)

# Graphics: D3D8 → D3D11 compatibility layer
if(XBOXRECOMP_D3D11)
    add_subdirectory(src/d3d)
endif()

# LLE paths, compile-time optional (design principle A10): a title whose XDK
# libraries run at API level (src/xdk) builds without them.
option(XBOXRECOMP_LLE_APU "Build the MCPX APU emulation (and the old DirectSound mixer that needs it)" ON)
option(XBOXRECOMP_LLE_USB "Build the OHCI/USB host controller emulation" ON)

if(XBOXRECOMP_LLE_APU)
    # Audio: DirectSound → software mixer compatibility
    add_subdirectory(src/audio)

    # Audio: MCPX APU hardware emulation (voice processor, DSP)
    add_subdirectory(src/apu)
endif()

# GPU: NV2A hardware emulation (register handlers, MMIO, push buffer)
if(XBOXRECOMP_D3D11)
    add_subdirectory(src/nv2a)
endif()

# USB: MCPX OHCI host controllers. A title's XAPI is statically linked and
# drives these registers directly, so a pad only exists if they do.
if(XBOXRECOMP_LLE_USB)
    add_subdirectory(src/usb)
endif()

# Input: host pads (SDL3), keyboard, pad script
add_subdirectory(src/input)

# The user's media folder: custom soundtracks (scan, playlists, decoders)
add_subdirectory(src/media)

# XDK library HLE layer (XInput at API level; DirectSound next)
add_subdirectory(src/xdk)

# Shared FMV player (Media Foundation -> D3D8 texture)
add_subdirectory(src/video)

# Vulkan renderer for the pushbuffer executor (nv2a_backend.h), from
# burnout3-sw. Needs glslang and SDL3 as CMake packages (CMAKE_PREFIX_PATH)
# and the Vulkan headers/loader; a game project registers it with
# nv2a_vk_install() and runs with RECOMP_PB_EXEC=1.
option(XBOXRECOMP_VULKAN "Build the Vulkan renderer (nv2a_vk)" OFF)
if(XBOXRECOMP_VULKAN)
    add_subdirectory(src/nv2a_vk)
    # The shader module's packs (media/shaders zips -> data/shaders, preset index); SDL3 like nv2a_vk.
    add_subdirectory(src/shaderpack)
    # The optional boot animation (media/boot.mpg, pl_mpeg); SDL3 like nv2a_vk.
    add_subdirectory(src/bootvideo)
endif()
# The port installer (game_setup.h); its SDL3 window (xbox_install_ui) when SDL3 is there (the Vulkan build).
add_subdirectory(src/install)
# The player's one exe (release; launcher.h): needs the installer window and the zip reader (Vulkan build).
if(XBOXRECOMP_VULKAN AND TARGET xbox_install_ui)
    add_subdirectory(src/launcher)
endif()

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
add_library(xboxrecomp INTERFACE)
target_link_libraries(xboxrecomp INTERFACE
    xbox_kernel
    xbox_input
    xbox_xdk
    xbox_video
)
if(XBOXRECOMP_LLE_APU)
    target_link_libraries(xboxrecomp INTERFACE xbox_dsound xbox_apu)
endif()
if(XBOXRECOMP_LLE_USB)
    target_link_libraries(xboxrecomp INTERFACE xbox_usb)
endif()
if(XBOXRECOMP_D3D11)
    target_link_libraries(xboxrecomp INTERFACE xbox_d3d8 xbox_nv2a)
endif()
if(XBOXRECOMP_VULKAN)
    target_link_libraries(xboxrecomp INTERFACE xbox_nv2a_vk)
endif()

