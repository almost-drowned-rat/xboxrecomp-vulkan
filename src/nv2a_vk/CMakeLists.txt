cmake_minimum_required(VERSION 3.20)

# Vulkan 1.3 renderer for the pushbuffer executor (XBOXRECOMP_VULKAN=ON).
# From burnout3-sw (antoxa2584x/burnout3-sw 0bf84b5, xboxrecomp/src/nv2a_vk),
# MIT. Shares the shader generators with that fork's nv2a_gl, in their Vulkan
# dialect, and compiles them with glslang at run time.
#
# Windows and Linux: SDL3 for the window and the Vulkan surface, glslang's C
# interface without the optimizer, any Vulkan 1.3 driver. glslang is C++, so
# the final link needs the C++ runtime. A C-only game project links with the
# C driver, so the runtime is named here: the C++ compiler's own implicit link
# libraries, filtered to the C++ ones (c++ on llvm-mingw, stdc++ with GCC).
# The Switch build (NVK from mesa-switch) is kept in nv2a_vk.c but not wired
# up here.
enable_language(CXX)

add_library(xbox_nv2a_vk STATIC
    nv2a_vk.c
    range_alloc.c
    shader_pass.c
    ../nv2a_gl/gl_vsh.c
    ../nv2a_gl/gl_vsh_ff.c
    ../nv2a_gl/gl_psh.c
)
target_include_directories(xbox_nv2a_vk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# SMAA (antialiasing option): the reference implementation, vendored unchanged
# in third_party/smaa (iryoku/smaa 71c806a, MIT; see NOTICE). Its shader source
# goes into the binary as bytes (smaa_hlsl.h, made here at configure time) and
# is compiled with glslang at run time like every other shader; AreaTex.h /
# SearchTex.h are C arrays already. No library.
set(_smaa_src ${CMAKE_CURRENT_SOURCE_DIR}/third_party/smaa/SMAA.hlsl)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_smaa_src})
file(READ ${_smaa_src} _smaa_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _smaa_hex "${_smaa_hex}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/smaa_hlsl.h
     "/* generated from third_party/smaa/SMAA.hlsl (src/nv2a_vk/CMakeLists.txt) */\n"
     "static const char s_smaa_hlsl[] = { ${_smaa_hex} 0 };\n")
target_include_directories(xbox_nv2a_vk PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Slang shaders (shader_pass.c): librashader's C header only, vendored in
# third_party/librashader (SnowflakePowered/librashader v0.12.0, MIT; see
# NOTICE). The library itself (MPL-2.0 / GPLv3) is the user's file next to the
# exe, loaded at run time with SDL_LoadObject -- never linked, built or shipped.

find_package(glslang CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
target_link_libraries(xbox_nv2a_vk PUBLIC
    glslang::glslang glslang::glslang-default-resource-limits
    SDL3::SDL3 Vulkan::Vulkan)
set(_nv2a_vk_cxx_rt "")
foreach(_lib IN LISTS CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
    if(_lib MATCHES "^(c[+][+]|c[+][+]abi|stdc[+][+]|unwind)$")
        list(APPEND _nv2a_vk_cxx_rt ${_lib})
    endif()
endforeach()
message(STATUS "nv2a_vk: C++ runtime for C consumers: ${_nv2a_vk_cxx_rt}")
target_link_libraries(xbox_nv2a_vk PUBLIC ${_nv2a_vk_cxx_rt})

if(NOT MSVC)
    target_compile_options(xbox_nv2a_vk PRIVATE -Wall -Wextra -Wno-unused-parameter
        -Wno-missing-field-initializers -O2)
endif()
