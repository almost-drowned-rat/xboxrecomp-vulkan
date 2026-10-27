# The user's media folder: custom soundtracks (scan, playlists, decoders).
# Pure host code, no guest knowledge; src/xdk/xdk_soundtrack.c puts it behind
# the XDK soundtrack API. Decoders vendored in third_party/ (see NOTICE).

add_library(xbox_media STATIC
    media_scan.c
    media_decode.c
    media_playlist.c
)

target_include_directories(xbox_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT WIN32)
    target_link_libraries(xbox_media PUBLIC m)
endif()

if(MSVC)
    target_compile_options(xbox_media PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_media PRIVATE -Wall -Wextra -O2)
endif()
