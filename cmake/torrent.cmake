# The torrent module (desktops): libtorrent-rasterbar (BSD-3-Clause) with Boost's headers (BSL-1.0) and OpenSSL
# (Apache-2.0; HTTPS trackers and encrypted peers). Everything is linked into the one module file,
# lunashare_torrent.dll / liblunashare_torrent.so; without that file LunaShare simply has no torrents.
#
# OpenSSL comes from the system (Linux: libssl-dev; Windows: OPENSSL_ROOT_DIR), linked statically. When it isn't found
# the module is still built, without TLS (UDP / HTTP trackers, DHT and magnet links work).

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# ---- Boost: only its headers (libtorrent needs no compiled Boost library since 1.69)
FetchContent_Declare(ls_boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-b2-nodocs.tar.xz
    URL_HASH SHA256=a4d99d032ab74c9c5e76eddcecc4489134282245fffa7e079c5804b92b45f51d
    SOURCE_SUBDIR do-not-build)
FetchContent_MakeAvailable(ls_boost)
# (libtorrent asks find_package(Boost): answered with a config file of ours that points at those headers)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ls_boost_config/BoostConfig.cmake
"set(Boost_FOUND TRUE)
set(Boost_VERSION 1.86.0)
set(Boost_VERSION_STRING 1.86.0)
set(Boost_MAJOR_VERSION 1)
set(Boost_MINOR_VERSION 86)
set(Boost_SUBMINOR_VERSION 0)
set(Boost_INCLUDE_DIRS \"${ls_boost_SOURCE_DIR}\")
set(Boost_INCLUDE_DIR \"${ls_boost_SOURCE_DIR}\")
if(NOT TARGET Boost::headers)
    add_library(Boost::headers INTERFACE IMPORTED)
    set_target_properties(Boost::headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"${ls_boost_SOURCE_DIR}\")
    target_compile_definitions(Boost::headers INTERFACE BOOST_ALL_NO_LIB)
endif()
if(NOT TARGET Boost::boost)
    add_library(Boost::boost INTERFACE IMPORTED)
    target_link_libraries(Boost::boost INTERFACE Boost::headers)
endif()
")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ls_boost_config/BoostConfigVersion.cmake
"set(PACKAGE_VERSION 1.86.0)
set(PACKAGE_VERSION_COMPATIBLE TRUE)
")
set(Boost_DIR ${CMAKE_CURRENT_BINARY_DIR}/ls_boost_config CACHE PATH "" FORCE)
set(Boost_NO_BOOST_CMAKE OFF CACHE BOOL "" FORCE)

# ---- OpenSSL (static)
set(OPENSSL_USE_STATIC_LIBS TRUE)
if(MSVC)
    set(OPENSSL_MSVC_STATIC_RT TRUE)
endif()
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "torrent module: OpenSSL ${OPENSSL_VERSION} (${OPENSSL_SSL_LIBRARY})")
else()
    message(WARNING "torrent module: OpenSSL not found - built without TLS (no HTTPS trackers)")
    set(encryption OFF CACHE BOOL "" FORCE)
endif()

# ---- libtorrent
foreach(o build_tests build_examples build_tools python-bindings deprecated-functions)
    set(${o} OFF CACHE BOOL "" FORCE)
endforeach()
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded(Debug)?$")
    set(static_runtime ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(libtorrent GIT_REPOSITORY https://github.com/arvidn/libtorrent.git GIT_TAG v2.0.15 GIT_SHALLOW TRUE
    GIT_SUBMODULES deps/try_signal GIT_SUBMODULES_RECURSE FALSE)
ls_fetch_excluded(libtorrent)
set_target_properties(torrent-rasterbar PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(MSVC)
    target_compile_options(torrent-rasterbar PRIVATE /w /bigobj)
else()
    target_compile_options(torrent-rasterbar PRIVATE -w)
endif()

add_library(lunashare_torrent SHARED modules/torrent/torrent.cpp)
set_target_properties(lunashare_torrent PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(lunashare_torrent PRIVATE src/media_api ${nlohmann_json_SOURCE_DIR}/include)
target_link_libraries(lunashare_torrent PRIVATE torrent-rasterbar Threads::Threads)
if(MSVC)
    target_compile_options(lunashare_torrent PRIVATE /bigobj)
elseif(WIN32)
    target_link_options(lunashare_torrent PRIVATE -static)
elseif(NOT APPLE)
    target_link_options(lunashare_torrent PRIVATE -Wl,--exclude-libs,ALL)
endif()
