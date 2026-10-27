# The player's one exe (launcher.h; porting guide M7 "Release"): folders, the installer, the payload appended to
# the exe, the compiler fetched once, lift / compile / link, then the game. SDL3 only; the zip reader + tinfl from
# src/shaderpack, the installer + its window from src/install, the window icon registry from src/kernel
# (window_icon.c - the only member it pulls from that library). A port builds its own launcher exe from
# xr_launcher_main and its XrLauncherConfig (THPS 2X: launcher/main.c).

add_library(xbox_launcher STATIC
    launcher.c
    http_get.c
    sha256.c
)
target_include_directories(xbox_launcher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT TARGET SDL3::SDL3)                 # imported targets are directory-scoped (as in src/install)
    find_package(SDL3 CONFIG REQUIRED)
endif()
target_link_libraries(xbox_launcher PUBLIC xbox_install_ui xbox_install xbox_shaderpack xbox_kernel SDL3::SDL3)
if(WIN32)
    target_link_libraries(xbox_launcher PUBLIC winhttp)   # the compiler download (http_get.c)
endif()

if(MSVC)
    target_compile_options(xbox_launcher PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_launcher PRIVATE -Wall -Wextra -O2)
endif()
