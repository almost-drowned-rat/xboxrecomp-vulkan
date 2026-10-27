# Shared FMV player: Media Foundation decode onto a D3D8 texture, and the
# framebuffer window (fb_present.c, plain GDI) the pushbuffer executor uses.
# XBOXRECOMP_D3D11=OFF: only the framebuffer window; the player becomes
# video_stub.c (host movie playback, RECOMP_FMV_HOST, is then unavailable).
if(XBOXRECOMP_D3D11)
    add_library(xbox_video STATIC
        video_player.c
        video_pump.c
        fb_present.c
    )
else()
    add_library(xbox_video STATIC
        video_stub.c
        fb_present.c
    )
endif()
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
if(XBOXRECOMP_D3D11)
    target_link_libraries(xbox_video PUBLIC xbox_d3d8)
    if(WIN32)
        target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
    endif()
endif()
