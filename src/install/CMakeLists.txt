# The port installer (game_setup.h): the game's files from the player's disc image into <layout>/game/ on the
# first start. xbox_install is plain C with no dependency (tests run it headless); xbox_install_ui adds the
# SDL3 progress window.
add_library(xbox_install STATIC xdvdfs.c game_setup.c)
target_include_directories(xbox_install PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# SDL3::SDL3 from nv2a_vk's find_package is not visible in this folder (imported targets are directory-scoped).
if(NOT TARGET SDL3::SDL3)
    find_package(SDL3 CONFIG QUIET)
endif()
if(TARGET SDL3::SDL3)
    add_library(xbox_install_ui STATIC game_setup_ui.c)
    target_include_directories(xbox_install_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(xbox_install_ui PUBLIC xbox_install SDL3::SDL3)
endif()
