cmake_minimum_required(VERSION 3.16)
project(LunaShare VERSION 1.7.2 LANGUAGES C CXX)
add_compile_definitions(LUNASHARE_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LUNASHARE_BUILD_APP   "Build the Dear ImGui application" ON)
option(LUNASHARE_BUILD_TESTS "Build headless engine tests" ON)
option(LUNASHARE_BUILD_SERVER "Build the lunashare-rendezvous server" ON)
option(LUNASHARE_BUILD_VOICE "Build the optional voice call module (separate library)" ON)
option(LUNASHARE_VIDEO_TEST_MODULE "Development: build lunashare_video with a test-pattern camera where no real backend exists" OFF)
set(LUNASHARE_FFMPEG_DIR "" CACHE PATH "FFmpeg 7.1 install for HEVC/AV1 decoding in the Windows video module (optional)")
option(LUNASHARE_BUILD_PLAYER "Build the optional player module (audio/video files in chat)" ON)
option(LUNASHARE_BUILD_TORRENT "Build the optional torrent module (desktops: libtorrent, Boost headers, OpenSSL)" ON)
option(LUNASHARE_BUILD_VIDEO "Build the optional video call module (separate library, Windows/Android)" ON)
option(LUNASHARE_SYSTEM_SDL  "Use an installed SDL2 instead of fetching it" OFF)
set(LUNASHARE_GL_API "GL3" CACHE STRING "Renderer API: GL3 (desktop), GLES3 (Android/iOS), GLES2")

include(FetchContent)
# Some dependencies are added with their own CMake but nothing of theirs is built unless something here links it
# (FetchContent_MakeAvailable gets EXCLUDE_FROM_ALL only with CMake 3.28; Android's is 3.22)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
macro(ls_fetch_excluded name)
    FetchContent_GetProperties(${name})
    if(NOT ${name}_POPULATED)
        FetchContent_Populate(${name})
        set(_ls_shared ${BUILD_SHARED_LIBS})
        set(BUILD_SHARED_LIBS OFF)
        add_subdirectory(${${name}_SOURCE_DIR} ${${name}_BINARY_DIR} EXCLUDE_FROM_ALL)
        set(BUILD_SHARED_LIBS ${_ls_shared})
    endif()
endmacro()
# Offline / pre-downloaded builds: pass -DFETCHCONTENT_SOURCE_DIR_IMGUI=/path etc.
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.12.0 GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_Declare(monocypher GIT_REPOSITORY https://github.com/LoupVaillant/Monocypher.git GIT_TAG 4.0.3 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(monocypher)  # no CMakeLists.txt upstream: only populates the sources
# KCP (reliable UDP for internet connections). Its CMakeLists needs CMake 4, so only the sources are used.
FetchContent_Declare(kcp GIT_REPOSITORY https://github.com/skywind3000/kcp.git GIT_TAG 2.1.1 GIT_SHALLOW TRUE SOURCE_SUBDIR do-not-build)
FetchContent_MakeAvailable(kcp)

# ---------------------------------------------------------------- core library
add_library(lunashare_core STATIC
    src/core/platform.cpp
    src/core/mp4_writer.cpp
    src/core/hash.cpp
    src/core/net.cpp
    src/core/protocol.cpp
    src/core/model.cpp
    src/core/bluetooth.cpp
    src/core/engine.cpp
    src/core/library.cpp
    src/core/groups.cpp
    src/core/group_calls.cpp
    src/core/watch.cpp
    src/core/folder_sync.cpp
    src/core/listen.cpp
    src/core/chat_archive.cpp
    src/core/crypto.cpp
    src/core/mdns.cpp
    src/core/wan.cpp
    src/core/upnp.cpp
    src/core/rendezvous_server.cpp
    src/core/link_crypto.cpp
    src/core/link_page.cpp
    src/core/share_links.cpp
    ${kcp_SOURCE_DIR}/ikcp.c
    ${monocypher_SOURCE_DIR}/src/monocypher.c
    ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c
)
target_include_directories(lunashare_core PUBLIC src/core PRIVATE ${monocypher_SOURCE_DIR}/src ${monocypher_SOURCE_DIR}/src/optional ${kcp_SOURCE_DIR})
target_link_libraries(lunashare_core PUBLIC nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(lunashare_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(lunashare_core PUBLIC ws2_32 iphlpapi shell32 ole32 bcrypt)
    target_compile_definitions(lunashare_core PUBLIC _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
if(MSVC)
    target_compile_options(lunashare_core PUBLIC /utf-8)
else()
    target_compile_options(lunashare_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-result)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(lunashare_core PUBLIC stdc++fs)
endif()

# ---------------------------------------------------------------- optional voice module
# A separate shared library (Opus + SpeexDSP). The app loads it at runtime if it sits next to the executable;
# without it LunaShare is chat + files only. The DSP code is also built as a static lib for the tests.
if(LUNASHARE_BUILD_VOICE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(opus GIT_REPOSITORY https://github.com/xiph/opus.git GIT_TAG v1.5.2 GIT_SHALLOW TRUE)
    FetchContent_Declare(speexdsp GIT_REPOSITORY https://github.com/xiph/speexdsp.git GIT_TAG SpeexDSP-1.2.1 GIT_SHALLOW TRUE SOURCE_SUBDIR do-not-build)
    # RNNoise (a small neural network) for noise suppression: v0.1.1 carries its model in the source
    FetchContent_Declare(rnnoise GIT_REPOSITORY https://github.com/xiph/rnnoise.git GIT_TAG v0.1.1 GIT_SHALLOW TRUE SOURCE_SUBDIR do-not-build)
    FetchContent_MakeAvailable(opus speexdsp rnnoise)
    set(LS_RNN_SRC ${rnnoise_SOURCE_DIR}/src/denoise.c ${rnnoise_SOURCE_DIR}/src/rnn.c ${rnnoise_SOURCE_DIR}/src/rnn_data.c
        ${rnnoise_SOURCE_DIR}/src/kiss_fft.c)
    if(MSVC)
        # (MSVC's C has no variable-length arrays: pitch.c and celt_lpc.c get copies with _alloca)
        foreach(f pitch celt_lpc)
            file(READ ${rnnoise_SOURCE_DIR}/src/${f}.c src)
            string(REGEX REPLACE "(opus_val(16|32)) ([a-z_0-9]+)\\[([^];]*[a-z][^];]*)\\];" "\\1 *\\3 = (\\1*)_alloca(sizeof(\\1)*(\\4));" src "${src}")
            file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/rnnoise_msvc/${f}.c "#include <malloc.h>\n${src}")
            list(APPEND LS_RNN_SRC ${CMAKE_CURRENT_BINARY_DIR}/rnnoise_msvc/${f}.c)
        endforeach()
    else()
        list(APPEND LS_RNN_SRC ${rnnoise_SOURCE_DIR}/src/pitch.c ${rnnoise_SOURCE_DIR}/src/celt_lpc.c)
    endif()
    add_library(lunashare_rnnoise STATIC ${LS_RNN_SRC})
    target_include_directories(lunashare_rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include)
    target_include_directories(lunashare_rnnoise PRIVATE ${rnnoise_SOURCE_DIR}/src)
    set_target_properties(lunashare_rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(lunashare_rnnoise PRIVATE /w)
        target_compile_definitions(lunashare_rnnoise PRIVATE _USE_MATH_DEFINES)
    else()
        target_compile_options(lunashare_rnnoise PRIVATE -w)
        target_link_libraries(lunashare_rnnoise PUBLIC m)
    endif()
    file(WRITE ${CMAKE_BINARY_DIR}/speexdsp_cfg/speexdsp_config_types.h
        "#ifndef __SPEEX_TYPES_H__\n#define __SPEEX_TYPES_H__\n#include <stdint.h>\ntypedef int16_t spx_int16_t;\ntypedef uint16_t spx_uint16_t;\ntypedef int32_t spx_int32_t;\ntypedef uint32_t spx_uint32_t;\n#endif\n")
    set(SPEEXDSP_SOURCES
        ${speexdsp_SOURCE_DIR}/libspeexdsp/mdf.c ${speexdsp_SOURCE_DIR}/libspeexdsp/preprocess.c
        ${speexdsp_SOURCE_DIR}/libspeexdsp/fftwrap.c ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fft.c
        ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fftr.c ${speexdsp_SOURCE_DIR}/libspeexdsp/filterbank.c)
    add_library(lunashare_speexdsp STATIC ${SPEEXDSP_SOURCES})
    target_compile_definitions(lunashare_speexdsp PUBLIC FLOATING_POINT USE_KISS_FFT EXPORT= DISABLE_WARNINGS)
    target_include_directories(lunashare_speexdsp PUBLIC ${speexdsp_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/speexdsp_cfg)
    if(NOT MSVC)
        target_compile_options(lunashare_speexdsp PRIVATE -w)
        target_link_libraries(lunashare_speexdsp PUBLIC m)
    endif()

    # WebRTC's echo canceller AEC3 (as in Chrome), vendored: only the audio processing parts
    add_subdirectory(third_party/webrtc_apm)

    add_library(lunashare_media SHARED modules/voice/voice.cpp)
    set_target_properties(lunashare_media PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_include_directories(lunashare_media PRIVATE src/media_api)
    target_link_libraries(lunashare_media PRIVATE opus lunashare_speexdsp lunashare_rnnoise webrtc_apm Threads::Threads)
    if(WIN32 AND NOT MSVC)
        target_link_options(lunashare_media PRIVATE -static)
    elseif(APPLE)
        target_link_options(lunashare_media PRIVATE -Wl,-exported_symbol,_lunashare_media_module)
    elseif(NOT WIN32)
        target_link_options(lunashare_media PRIVATE -Wl,--exclude-libs,ALL)  # export only lunashare_media_module
    endif()

    add_library(lunashare_voice_static STATIC modules/voice/voice.cpp)  # same code, for tests
    target_include_directories(lunashare_voice_static PUBLIC src/media_api)
    target_link_libraries(lunashare_voice_static PUBLIC opus lunashare_speexdsp lunashare_rnnoise webrtc_apm Threads::Threads)
endif()

# ---------------------------------------------------------------- optional video module
# lunashare_video: camera + H.264 through the platform's own (hardware) codecs - Media Foundation on Windows,
# Camera2 + MediaCodec on Android (Java side: android/app/src/calls/java). No codec library is bundled.
# The portable core is also built with a synthetic test backend for the tests on every platform.
if(LUNASHARE_BUILD_VIDEO)
    set(LSV_PLATFORM "")
    if(WIN32)
        # HEVC/AV1 decoding through FFmpeg (avcodec-61/avutil-59 DLLs next to the module, loaded at run time).
        # LUNASHARE_FFMPEG_DIR = an FFmpeg 7.1 install (include/ + bin/); without it only H.264 is decoded.
        if(LUNASHARE_FFMPEG_DIR AND EXISTS "${LUNASHARE_FFMPEG_DIR}/include/libavcodec/avcodec.h")
            set(LSV_PLATFORM modules/video/video_win.cpp modules/video/ffdec.cpp)
            set(LSV_FFMPEG_INCLUDE "${LUNASHARE_FFMPEG_DIR}/include")
        else()
            set(LSV_PLATFORM modules/video/video_win.cpp modules/video/ffdec_stub.cpp)
        endif()
    elseif(ANDROID)
        set(LSV_PLATFORM modules/video/video_android.cpp)
    elseif(UNIX AND NOT APPLE AND LUNASHARE_FFMPEG_DIR AND EXISTS "${LUNASHARE_FFMPEG_DIR}/include/libavcodec/avcodec.h"
           AND NOT LUNASHARE_VIDEO_TEST_MODULE)
        # Linux: V4L2 cameras, x264 / FFmpeg codecs (the bundled FFmpeg in modules/), v4l2loopback virtual camera
        set(LSV_PLATFORM modules/video/video_linux.cpp modules/video/ffdec.cpp)
        set(LSV_FFMPEG_INCLUDE "${LUNASHARE_FFMPEG_DIR}/include")
    elseif(LUNASHARE_VIDEO_TEST_MODULE)
        set(LSV_PLATFORM modules/video/video_fake.cpp)  # development: test-pattern camera, no real codec
    endif()
    if(LSV_PLATFORM)
        add_library(lunashare_video SHARED modules/video/video_core.cpp ${LSV_PLATFORM})
        set_target_properties(lunashare_video PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
        target_include_directories(lunashare_video PRIVATE src/media_api modules/video)
        if(LSV_FFMPEG_INCLUDE)
            target_include_directories(lunashare_video PRIVATE ${LSV_FFMPEG_INCLUDE})
        endif()
        target_link_libraries(lunashare_video PRIVATE Threads::Threads)
        if(WIN32)
            target_include_directories(lunashare_video PRIVATE modules/vcam)
            target_link_libraries(lunashare_video PRIVATE mfplat mf mfreadwrite mfuuid ole32 oleaut32 strmiids wmcodecdspuuid d3d11 shell32 advapi32)
            if(NOT MSVC)
                target_link_options(lunashare_video PRIVATE -static)
            endif()
        elseif(ANDROID)
            target_link_libraries(lunashare_video PRIVATE log)
            target_link_options(lunashare_video PRIVATE -Wl,--exclude-libs,ALL)
        else()
            target_link_libraries(lunashare_video PRIVATE ${CMAKE_DL_LIBS})
            target_link_options(lunashare_video PRIVATE -Wl,--exclude-libs,ALL)
        endif()
    endif()
    if(WIN32)
        # Windows 11 virtual camera ("LunaShare Camera"): a media source the camera Frame Server service loads.
        # Fully static so the service can load it without the app's runtime DLLs.
        add_library(lunashare_vcam SHARED modules/vcam/vcam_source.cpp modules/vcam/vcam.def)
        set_target_properties(lunashare_vcam PROPERTIES PREFIX "" OUTPUT_NAME lunashare_vcam
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_libraries(lunashare_vcam PRIVATE mfplat mfuuid ole32 advapi32)
        if(NOT MSVC)
            target_link_options(lunashare_vcam PRIVATE -static)
        endif()
    endif()
    add_library(lunashare_video_test STATIC modules/video/video_core.cpp modules/video/video_fake.cpp)
    target_include_directories(lunashare_video_test PUBLIC src/media_api modules/video)
    target_link_libraries(lunashare_video_test PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------- optional player module
# lunashare_player: plays audio/video files from the chat. Android uses the platform decoders (Java side:
# android/app/src/main/java/.../PlayerEngine.java), desktop uses FFmpeg (LUNASHARE_FFMPEG_DIR, DLLs next to it).
if(LUNASHARE_BUILD_PLAYER)
    set(LSP_PLATFORM "")
    if(ANDROID)
        set(LSP_PLATFORM modules/player/player_android.cpp)
        # FFmpeg for sound and subtitles (built by CI per ABI: <dir>/<abi>/include, lib/*.a); without it only what
        # the phone decodes itself plays
        set(LUNASHARE_FFMPEG_ANDROID "" CACHE PATH "FFmpeg static libraries for Android (sound + subtitle decoders)")
        set(LSP_FFA "${LUNASHARE_FFMPEG_ANDROID}/${ANDROID_ABI}")
        if(LUNASHARE_FFMPEG_ANDROID AND EXISTS "${LSP_FFA}/lib/libavformat.a")
            list(APPEND LSP_PLATFORM modules/player/player_ff.cpp)
            set(LSP_FF_ANDROID ON)
            message(STATUS "player: FFmpeg sound/subtitles for ${ANDROID_ABI}")
        endif()
    elseif(LUNASHARE_FFMPEG_DIR AND EXISTS "${LUNASHARE_FFMPEG_DIR}/include/libavformat/avformat.h")
        set(LSP_PLATFORM modules/player/player_ff.cpp)
    endif()
    if(LSP_PLATFORM)
        add_library(lunashare_player SHARED modules/player/player_core.cpp ${LSP_PLATFORM})
        set_target_properties(lunashare_player PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
        target_include_directories(lunashare_player PRIVATE src/media_api modules/player)
        target_link_libraries(lunashare_player PRIVATE Threads::Threads)
        if(NOT ANDROID)
            target_include_directories(lunashare_player PRIVATE "${LUNASHARE_FFMPEG_DIR}/include")
        elseif(LSP_FF_ANDROID)
            target_include_directories(lunashare_player PRIVATE "${LSP_FFA}/include")
            target_compile_definitions(lunashare_player PRIVATE LSP_FF_STATIC LSP_FF_SIDE)
            target_link_libraries(lunashare_player PRIVATE "${LSP_FFA}/lib/libavformat.a" "${LSP_FFA}/lib/libavcodec.a"
                "${LSP_FFA}/lib/libswresample.a" "${LSP_FFA}/lib/libavutil.a" z m)
            target_link_options(lunashare_player PRIVATE -Wl,--exclude-libs,ALL)
        endif()
        if(WIN32 AND NOT MSVC)
            target_link_options(lunashare_player PRIVATE -static)
        elseif(APPLE)
            target_link_options(lunashare_player PRIVATE -Wl,-exported_symbol,_lunashare_player_module)
        elseif(NOT WIN32)
            target_link_libraries(lunashare_player PRIVATE ${CMAKE_DL_LIBS})
            target_link_options(lunashare_player PRIVATE -Wl,--exclude-libs,ALL)
        endif()
    endif()
endif()

# ---------------------------------------------------------------- optional emoji module
# lunashare_emoji: colour emoji drawn with the system's emoji font (Windows: DirectWrite, Android: android.graphics
# via android/app/src/main/java/.../EmojiRenderer.java, elsewhere: FreeType) and the picker's emoji list.
# lunashare_torrent: torrents on desktops (see cmake/torrent.cmake)
if(LUNASHARE_BUILD_TORRENT AND LUNASHARE_BUILD_APP AND NOT ANDROID AND NOT APPLE AND NOT IOS AND NOT CMAKE_SYSTEM_NAME MATCHES "BSD")
    include(cmake/torrent.cmake)
endif()

option(LUNASHARE_BUILD_EMOJI "Build the optional emoji module" ON)
if(LUNASHARE_BUILD_EMOJI)
    set(LSE_PLATFORM "")
    if(WIN32)
        set(LSE_PLATFORM modules/emoji/emoji_win.cpp)
    elseif(ANDROID)
        set(LSE_PLATFORM modules/emoji/emoji_android.cpp)
    else()
        find_package(Freetype QUIET)
        if(FREETYPE_FOUND)
            set(LSE_PLATFORM modules/emoji/emoji_ft.cpp)
        endif()
    endif()
    if(LSE_PLATFORM)
        add_library(lunashare_emoji SHARED modules/emoji/emoji_core.cpp ${LSE_PLATFORM})
        set_target_properties(lunashare_emoji PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
        target_include_directories(lunashare_emoji PRIVATE src/media_api modules/emoji)
        if(WIN32)
            target_link_libraries(lunashare_emoji PRIVATE d2d1 dwrite windowscodecs ole32)
            if(NOT MSVC)
                target_link_options(lunashare_emoji PRIVATE -static)
            endif()
        elseif(ANDROID)
            target_link_libraries(lunashare_emoji PRIVATE log)
        else()
            target_link_libraries(lunashare_emoji PRIVATE Freetype::Freetype)
            if(APPLE)
                target_link_options(lunashare_emoji PRIVATE -Wl,-exported_symbol,_lunashare_emoji_module)
            else()
                target_link_options(lunashare_emoji PRIVATE -Wl,--exclude-libs,ALL)
            endif()
        endif()
    endif()
endif()

# ---------------------------------------------------------------- rendezvous server
if((LUNASHARE_BUILD_SERVER OR LUNASHARE_BUILD_TESTS) AND NOT ANDROID AND NOT IOS)
    # "a link for anyone" pages: HTTPS with OpenSSL when it's there (static), plain HTTP (behind a proxy) without
    add_library(lunashare_linkserver STATIC src/core/link_server.cpp)
    target_link_libraries(lunashare_linkserver PUBLIC lunashare_core)
    if(NOT OpenSSL_FOUND)
        set(OPENSSL_USE_STATIC_LIBS TRUE)
        if(MSVC)
            set(OPENSSL_MSVC_STATIC_RT TRUE)
        endif()
        find_package(OpenSSL QUIET)
    endif()
    if(OpenSSL_FOUND)
        target_compile_definitions(lunashare_linkserver PRIVATE LUNASHARE_LINK_TLS=1)
        target_link_libraries(lunashare_linkserver PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        if(WIN32)
            target_link_libraries(lunashare_linkserver PRIVATE crypt32 ws2_32 user32 advapi32)
        elseif(NOT APPLE)
            target_link_libraries(lunashare_linkserver PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
        endif()
        message(STATUS "link server: HTTPS with OpenSSL ${OPENSSL_VERSION}")
    else()
        message(STATUS "link server: OpenSSL not found - plain HTTP only (put it behind a reverse proxy for HTTPS)")
    endif()
endif()
if(LUNASHARE_BUILD_SERVER AND NOT ANDROID AND NOT IOS)
    add_executable(lunashare-rendezvous server/main.cpp)
    target_link_libraries(lunashare-rendezvous PRIVATE lunashare_core lunashare_linkserver)
    install(TARGETS lunashare-rendezvous RUNTIME DESTINATION bin)
endif()

# ---------------------------------------------------------------- tests
if(LUNASHARE_BUILD_TESTS AND NOT ANDROID AND NOT IOS)
    add_executable(lunashare_tests tests/test_engine.cpp)
    target_link_libraries(lunashare_tests PRIVATE lunashare_core lunashare_linkserver)
    enable_testing()
    add_test(NAME engine COMMAND lunashare_tests)
    if(LUNASHARE_BUILD_VOICE)
        add_executable(lunashare_voice_tests tests/test_voice.cpp)
        target_link_libraries(lunashare_voice_tests PRIVATE lunashare_voice_static lunashare_core opus)
        add_test(NAME voice COMMAND lunashare_voice_tests)
    endif()
    if(LUNASHARE_BUILD_VIDEO AND NOT ANDROID)
        add_executable(lunashare_video_tests tests/test_video.cpp)
        target_link_libraries(lunashare_video_tests PRIVATE lunashare_video_test)
        add_test(NAME video COMMAND lunashare_video_tests)
    endif()
    add_executable(lunashare_demo_peer tests/demo_peer.cpp)
    target_link_libraries(lunashare_demo_peer PRIVATE lunashare_core)
    if(LUNASHARE_BUILD_VOICE)
        target_link_libraries(lunashare_demo_peer PRIVATE lunashare_voice_static)
        target_compile_definitions(lunashare_demo_peer PRIVATE LS_DEMO_VOICE)
    endif()
    if(LUNASHARE_BUILD_VIDEO)
        target_link_libraries(lunashare_demo_peer PRIVATE lunashare_video_test)
        target_compile_definitions(lunashare_demo_peer PRIVATE LS_DEMO_VIDEO)
    endif()
endif()

# ---------------------------------------------------------------- application
if(LUNASHARE_BUILD_APP)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.93.0 GIT_SHALLOW TRUE)
    FetchContent_Declare(qrcodegen GIT_REPOSITORY https://github.com/nayuki/QR-Code-generator.git GIT_TAG v1.8.0 GIT_SHALLOW TRUE)
    FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5)
    # quirc (ISC): reads QR codes from camera pictures (verifying a contact by scanning their code)
    FetchContent_Declare(quirc GIT_REPOSITORY https://github.com/dlbeer/quirc.git GIT_TAG v1.2 GIT_SHALLOW TRUE SOURCE_SUBDIR do-not-build)
    FetchContent_MakeAvailable(imgui qrcodegen stb quirc)
    # libwebp (BSD): WebP pictures (the gallery, comics, the viewer) - only its decoder is built
    FetchContent_Declare(libwebp GIT_REPOSITORY https://github.com/webmproject/libwebp.git GIT_TAG v1.6.0 GIT_SHALLOW TRUE)
    foreach(o ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS)
        set(WEBP_BUILD_${o} OFF CACHE BOOL "" FORCE)
    endforeach()
    ls_fetch_excluded(libwebp)
    include(cmake/archives.cmake)
    if(ANDROID OR IOS)
        set(LUNASHARE_GL_API "GLES3")
    endif()

    if(LUNASHARE_SYSTEM_SDL)
        find_package(SDL2 REQUIRED)
    else()
        # (Android: calls use the voice stream and the voice-communication microphone - see the patch)
        FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-2.32.8 GIT_SHALLOW TRUE
            PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/sdl2-android-voice-audio.patch
                          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        if(ANDROID)
            set(SDL_SHARED ON CACHE BOOL "" FORCE)
            set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        else()
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
        endif()
        if(FETCHCONTENT_SOURCE_DIR_SDL2)  # (Android: gradle's own copy - FetchContent runs no PATCH_COMMAND on it)
            execute_process(COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/sdl2-android-voice-audio.patch
                                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake
                            WORKING_DIRECTORY ${FETCHCONTENT_SOURCE_DIR_SDL2} RESULT_VARIABLE sdl_patch)
            if(NOT sdl_patch EQUAL 0)
                message(FATAL_ERROR "could not patch SDL in ${FETCHCONTENT_SOURCE_DIR_SDL2}")
            endif()
        endif()
        FetchContent_MakeAvailable(SDL2)
    endif()

    set(APP_SOURCES
        src/app/main.cpp
        src/app/ui.cpp
        src/app/ui_widgets.cpp
        src/app/file_browser.cpp
        src/app/android_bridge.cpp
        src/app/ui_viewer.cpp
        src/app/ui_account.cpp
        src/app/ui_settings.cpp
        src/app/ui_library.cpp
        src/app/ui_groups.cpp
        src/app/ui_watch.cpp
        src/app/ui_listen.cpp
        src/app/ui_movies.cpp
        src/app/ui_archive.cpp
        src/app/ui_sleep.cpp
        src/app/ui_sync.cpp
        src/app/ui_links.cpp
        src/app/update.cpp
        src/app/ui_lock.cpp
        src/app/ui_polls.cpp
        src/app/instance.cpp
        src/app/ui_gallery.cpp
        src/app/ui_welcome.cpp
        src/app/archives.cpp
        src/app/ui_archives.cpp
        src/app/ui_comics.cpp
        src/app/torrent_host.cpp
        src/app/ui_torrents.cpp
        src/app/file_assoc.cpp
        src/app/ui_music.cpp
        src/app/music.cpp
        src/app/radio.cpp
        src/app/ui_radio.cpp
        src/app/net_stream.cpp
        src/app/notify.cpp
        src/app/tray.cpp
        src/app/image_cache.cpp
        src/app/avatar.cpp
        src/app/gl_funcs.cpp
        src/app/media_host.cpp src/app/player_host.cpp src/app/emoji_host.cpp src/app/i18n.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
        ${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.cpp
        ${quirc_SOURCE_DIR}/lib/quirc.c ${quirc_SOURCE_DIR}/lib/decode.c ${quirc_SOURCE_DIR}/lib/identify.c ${quirc_SOURCE_DIR}/lib/version_db.c
    )

    if(ANDROID)
        add_library(main SHARED ${APP_SOURCES})  # SDLActivity loads libmain.so
        set(APP_TARGET main)
        target_link_libraries(main PRIVATE GLESv3 EGL log android)
    else()
        add_executable(LunaShare WIN32 MACOSX_BUNDLE ${APP_SOURCES})
        set(APP_TARGET LunaShare)
        if(WIN32)
            target_sources(LunaShare PRIVATE res/lunashare.rc)
            target_link_libraries(LunaShare PRIVATE winhttp shell32 advapi32)  # the update check; "Open with" registration
            if(MSVC)
                target_link_options(LunaShare PRIVATE /MANIFEST:NO)  # our .rc already embeds the manifest
            endif()
        endif()
        # Desktop GL is loaded at runtime (imgui's GL loader + SDL_GL_GetProcAddress): no libGL link needed.
        target_link_libraries(LunaShare PRIVATE ${CMAKE_DL_LIBS})
        # internet radio over https (Linux / macOS: sockets + OpenSSL; Windows uses WinHTTP)
        if(NOT WIN32)
            if(NOT OpenSSL_FOUND)
                find_package(OpenSSL QUIET)
            endif()
            if(OpenSSL_FOUND)
                target_link_libraries(LunaShare PRIVATE OpenSSL::SSL OpenSSL::Crypto)
                target_compile_definitions(LunaShare PRIVATE LS_HTTP_OPENSSL)
            else()
                message(STATUS "internet radio: OpenSSL not found - http streams only")
            endif()
        endif()
        if(LUNASHARE_GL_API STREQUAL "GLES2" OR LUNASHARE_GL_API STREQUAL "GLES3")
            find_library(GLES_LIB NAMES GLESv2 libGLESv2.so.2 REQUIRED)
            target_link_libraries(LunaShare PRIVATE ${GLES_LIB})
        endif()
    endif()

    target_compile_definitions(${APP_TARGET} PRIVATE IMGUI_USE_WCHAR32)  # emoji and other characters beyond U+FFFF
    target_include_directories(${APP_TARGET} PRIVATE src/app src/media_api ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
        ${imgui_SOURCE_DIR}/misc/cpp ${qrcodegen_SOURCE_DIR}/cpp ${stb_SOURCE_DIR} ${quirc_SOURCE_DIR}/lib ${libwebp_SOURCE_DIR}/src)
    if(LUNASHARE_GL_API STREQUAL "GLES3")
        target_compile_definitions(${APP_TARGET} PRIVATE IMGUI_IMPL_OPENGL_ES3)
    elseif(LUNASHARE_GL_API STREQUAL "GLES2")
        target_compile_definitions(${APP_TARGET} PRIVATE IMGUI_IMPL_OPENGL_ES2)
        # SDL ships the Khronos GLES2/EGL headers
        target_include_directories(${APP_TARGET} PRIVATE ${sdl2_SOURCE_DIR}/src/video/khronos)
    endif()
    target_link_libraries(${APP_TARGET} PRIVATE lunashare_core webpdecoder archive_static)
    target_include_directories(${APP_TARGET} PRIVATE ${LS_ARCHIVE_INCLUDE})
    if(TARGET SDL2::SDL2main)
        target_link_libraries(${APP_TARGET} PRIVATE SDL2::SDL2main)
    endif()
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(${APP_TARGET} PRIVATE SDL2::SDL2-static)
    else()
        target_link_libraries(${APP_TARGET} PRIVATE SDL2::SDL2)
    endif()
endif()
