cmake_minimum_required(VERSION 3.20)

# @TITLE@ - static recompilation.
# After sp00nznet/xboxrecomp templates/new-game/CMakeLists.txt and
# refs/defjam_recomp/CMakeLists.txt. Build only via tools/build.ps1.

project(@NAME@_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
if(MINGW)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# Toolkit: the fork next to this repo, branch vulkan-backend (docs/toolkit.md).
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Generated C lives outside the repo (never committed): tools/lift.ps1 output.
set(RECOMP_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../private/@NAME@/gen" CACHE PATH
    "Directory holding the generated C (recomp_*.c, recomp_types.h, ...)")

# Record every indirect-branch target at runtime (icall_targets.dump) for the
# seeds feedback loop. Must be set before add_subdirectory (icall_feedback.c).
# Remove once function detection has converged (hot-path store).
add_compile_definitions(RECOMP_ICALL_FEEDBACK)

# Vulkan renderer (toolkit nv2a_vk, M2): glslang + SDL3 from tools/deps.ps1
# (CMAKE_PREFIX_PATH). OFF = the toolkit's CPU executor path, nothing drawn.
option(@PREFIX@_VULKAN "Render through the toolkit's Vulkan back end (nv2a_vk)" ON)
set(XBOXRECOMP_VULKAN ${@PREFIX@_VULKAN} CACHE BOOL "" FORCE)
# Vulkan only: no D3D11 libraries in the toolkit build (xbox_d3d8, xbox_nv2a,
# the D3D8 movie player) -- CLAUDE.md, M2 stage 5.
if(@PREFIX@_VULKAN)
    set(XBOXRECOMP_D3D11 OFF CACHE BOOL "" FORCE)
else()
    set(XBOXRECOMP_D3D11 ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# CONFIGURE_DEPENDS: a re-lift that adds/removes chunks is picked up.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${RECOMP_GEN_DIR}/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR "No generated C in ${RECOMP_GEN_DIR} - run tools/lift.ps1 first")
endif()

add_executable(${PROJECT_NAME}
    src/main.c
    src/recomp_manual.c
    src/input.c
    src/platform/host_win32.c
    ${RECOMP_GEN_SOURCES}
)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME @EXE@)
# Only the game goes into the portable install layout, not the toolkit's tests.
if(@PREFIX@_OUTPUT_DIR)
    set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${@PREFIX@_OUTPUT_DIR}")
endif()

# The toolkit libraries the game uses, named one by one instead of the
# umbrella target `xboxrecomp`: that one also pulls in xbox_d3d8 (D3D8 ->
# D3D11 for titles without their own D3D) and xbox_nv2a (an older D3D11
# PGRAPH path). @TITLE@'s D3D is lifted code driving the NV2A pushbuffer, so
# with nv2a_vk neither is needed -- Vulkan only (CLAUDE.md).
target_link_libraries(${PROJECT_NAME} PRIVATE
    xbox_kernel xbox_dsound xbox_input xbox_usb xbox_apu xbox_video)
if(@PREFIX@_VULKAN)
    target_link_libraries(${PROJECT_NAME} PRIVATE xbox_nv2a_vk)
    target_compile_definitions(${PROJECT_NAME} PRIVATE @PREFIX@_VULKAN=1)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)
endif()
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${RECOMP_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
)

# System libraries the toolkit's Windows runtime still needs. No D3D: the
# renderer is Vulkan (M2 stage 5 removed d3d11/dxgi/dxguid).
target_link_libraries(${PROJECT_NAME} PRIVATE
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>
    winmm
    dbghelp   # crash handler: host RIP -> generated sub_XXXXXXXX
    shell32   # CommandLineToArgvW: the command line as UTF-16
)

if(MINGW)
    # CodeView + .pdb (llvm-mingw/lld) so dbghelp in the crash handler can
    # name the generated function; DWARF is invisible to dbghelp.
    target_compile_options(${PROJECT_NAME} PRIVATE -g -gcodeview)
    # -static: libc++ (glslang) and winpthread linked in, so the portable
    # folder needs no extra DLLs (docs/toolkit.md, toolchain decision).
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--pdb= -Wl,--large-address-aware -static)
endif()
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE
        $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj $<$<CONFIG:Release>:/Zi>)
endif()
