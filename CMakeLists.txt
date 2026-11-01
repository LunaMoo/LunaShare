cmake_minimum_required(VERSION 3.16)
project(LunaShare VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LUNASHARE_BUILD_APP   "Build the Dear ImGui application" ON)
option(LUNASHARE_BUILD_TESTS "Build headless engine tests" ON)
option(LUNASHARE_SYSTEM_SDL  "Use an installed SDL2 instead of fetching it" OFF)
set(LUNASHARE_GL_API "GL3" CACHE STRING "Renderer API: GL3 (desktop), GLES3 (Android/iOS), GLES2")

include(FetchContent)
# Offline / pre-downloaded builds: pass -DFETCHCONTENT_SOURCE_DIR_IMGUI=/path etc.
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.12.0 GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_Declare(monocypher GIT_REPOSITORY https://github.com/LoupVaillant/Monocypher.git GIT_TAG 4.0.3 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(monocypher)  # no CMakeLists.txt upstream: only populates the sources

# ---------------------------------------------------------------- core library
add_library(lunashare_core STATIC
    src/core/platform.cpp
    src/core/hash.cpp
    src/core/net.cpp
    src/core/protocol.cpp
    src/core/model.cpp
    src/core/bluetooth.cpp
    src/core/engine.cpp
    src/core/crypto.cpp
    src/core/mdns.cpp
    ${monocypher_SOURCE_DIR}/src/monocypher.c
)
target_include_directories(lunashare_core PUBLIC src/core PRIVATE ${monocypher_SOURCE_DIR}/src)
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

# ---------------------------------------------------------------- tests
if(LUNASHARE_BUILD_TESTS AND NOT ANDROID AND NOT IOS)
    add_executable(lunashare_tests tests/test_engine.cpp)
    target_link_libraries(lunashare_tests PRIVATE lunashare_core)
    enable_testing()
    add_test(NAME engine COMMAND lunashare_tests)
    add_executable(lunashare_demo_peer tests/demo_peer.cpp)
    target_link_libraries(lunashare_demo_peer PRIVATE lunashare_core)
endif()

# ---------------------------------------------------------------- application
if(LUNASHARE_BUILD_APP)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.93.0 GIT_SHALLOW TRUE)
    FetchContent_Declare(qrcodegen GIT_REPOSITORY https://github.com/nayuki/QR-Code-generator.git GIT_TAG v1.8.0 GIT_SHALLOW TRUE)
    FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5)
    FetchContent_MakeAvailable(imgui qrcodegen stb)
    if(ANDROID OR IOS)
        set(LUNASHARE_GL_API "GLES3")
    endif()

    if(LUNASHARE_SYSTEM_SDL)
        find_package(SDL2 REQUIRED)
    else()
        FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-2.32.8 GIT_SHALLOW TRUE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        if(ANDROID)
            set(SDL_SHARED ON CACHE BOOL "" FORCE)
            set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        else()
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
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
        src/app/image_cache.cpp
        src/app/gl_funcs.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
        ${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.cpp
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
            if(MSVC)
                target_link_options(LunaShare PRIVATE /MANIFEST:NO)  # our .rc already embeds the manifest
            endif()
        endif()
        # Desktop GL is loaded at runtime (imgui's GL loader + SDL_GL_GetProcAddress): no libGL link needed.
        target_link_libraries(LunaShare PRIVATE ${CMAKE_DL_LIBS})
        if(LUNASHARE_GL_API STREQUAL "GLES2" OR LUNASHARE_GL_API STREQUAL "GLES3")
            find_library(GLES_LIB NAMES GLESv2 libGLESv2.so.2 REQUIRED)
            target_link_libraries(LunaShare PRIVATE ${GLES_LIB})
        endif()
    endif()

    target_include_directories(${APP_TARGET} PRIVATE src/app ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
        ${imgui_SOURCE_DIR}/misc/cpp ${qrcodegen_SOURCE_DIR}/cpp ${stb_SOURCE_DIR})
    if(LUNASHARE_GL_API STREQUAL "GLES3")
        target_compile_definitions(${APP_TARGET} PRIVATE IMGUI_IMPL_OPENGL_ES3)
    elseif(LUNASHARE_GL_API STREQUAL "GLES2")
        target_compile_definitions(${APP_TARGET} PRIVATE IMGUI_IMPL_OPENGL_ES2)
        # SDL ships the Khronos GLES2/EGL headers
        target_include_directories(${APP_TARGET} PRIVATE ${sdl2_SOURCE_DIR}/src/video/khronos)
    endif()
    target_link_libraries(${APP_TARGET} PRIVATE lunashare_core)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(${APP_TARGET} PRIVATE SDL2::SDL2main)
    endif()
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(${APP_TARGET} PRIVATE SDL2::SDL2-static)
    else()
        target_link_libraries(${APP_TARGET} PRIVATE SDL2::SDL2)
    endif()
endif()
