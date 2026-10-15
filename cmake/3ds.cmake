# Nintendo 3DS build of the app (configured with the devkitPro 3DS toolchain, see scripts/build-3ds.sh).

set(FFMPEG_3DS ${CMAKE_SOURCE_DIR}/third_party/ffmpeg-3ds)
if(NOT EXISTS ${FFMPEG_3DS}/lib/libavcodec.a)
    message(FATAL_ERROR "FFmpeg for 3DS missing: run scripts/build-ffmpeg.sh first")
endif()

target_compile_options(jelly_core PRIVATE -Wall -O2)

add_executable(jelly3ds
    app/main.cpp
    app/app.cpp
    app/gfx.cpp
    app/tasks.cpp
    app/image_cache.cpp
    app/widgets.cpp
    app/player/decoders.cpp
    app/player/player.cpp
    app/ui/common.cpp
    app/ui/menu.cpp
    app/ui/login.cpp
    app/ui/home.cpp
    app/ui/browse.cpp
    app/ui/download_options.cpp
    app/ui/player_screen.cpp
    app/ui/cover_page.cpp
    app/ui/library_screens.cpp
    app/ui/downloads_page.cpp
)
target_compile_options(jelly3ds PRIVATE -Wall -O2)
target_include_directories(jelly3ds PRIVATE app ${FFMPEG_3DS}/include)
target_link_directories(jelly3ds PRIVATE ${FFMPEG_3DS}/lib)
target_link_libraries(jelly3ds PRIVATE
    jelly_core
    avformat avcodec avutil
    curl mbedtls mbedx509 mbedcrypto z
    citro2d citro3d ctru m
)

ctr_generate_smdh(${CMAKE_CURRENT_BINARY_DIR}/jelly3ds.smdh
    NAME "Jelly3DS"
    DESCRIPTION "Jelly3DS"
    AUTHOR "Thomas Bonnici"
    ICON ${CMAKE_SOURCE_DIR}/resources/icon.png
)
ctr_create_3dsx(jelly3ds
    SMDH ${CMAKE_CURRENT_BINARY_DIR}/jelly3ds.smdh
    ROMFS ${CMAKE_SOURCE_DIR}/romfs
    OUTPUT ${CMAKE_BINARY_DIR}/Jelly3DS.3dsx
)

# Portlibs (curl, mbedtls, zlib) live outside the default include path.
target_include_directories(jelly_core PUBLIC ${DEVKITPRO}/portlibs/3ds/include)
target_link_directories(jelly3ds PRIVATE ${DEVKITPRO}/portlibs/3ds/lib)
