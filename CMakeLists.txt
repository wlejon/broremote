cmake_minimum_required(VERSION 3.24)

project(broremote VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/MP>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    )
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

option(BROREMOTE_BUILD_TESTS "Build the broremote test suite" ON)
option(BROREMOTE_BUILD_TOOLS "Build the broremote command-line tool" ON)

find_package(Threads REQUIRED)

# ---- library -------------------------------------------------------------------
# The core (wire, protocol, streams, server, client, the built-in Raw codec)
# needs nothing beyond the platform. Codec backends (VA-API, Media
# Foundation) register in src/codec_factory.cpp behind their own options.
set(BROREMOTE_SOURCES
    src/wire.cpp
    src/protocol.cpp
    src/codec_factory.cpp
    src/raw_codec.cpp
    src/paths_common.cpp
    src/server.cpp
    src/server_io.cpp
    src/server_encode.cpp
    src/client.cpp
    src/proxy.cpp
)
if(WIN32)
    list(APPEND BROREMOTE_SOURCES
        src/win/socket_win.cpp
        src/win/stream_win.cpp
        src/win/spawn_win.cpp
        src/win/paths_win.cpp
    )
else()
    list(APPEND BROREMOTE_SOURCES
        src/posix/socket_posix.cpp
        src/posix/stream_posix.cpp
        src/posix/spawn_posix.cpp
        src/posix/paths_posix.cpp
    )
endif()

add_library(broremote STATIC ${BROREMOTE_SOURCES})
add_library(broremote::broremote ALIAS broremote)
# MSVC writes a static library's compiler PDB as <target>.pdb, which in Debug
# is the same file as the link PDB of the broremote executable (OUTPUT_NAME
# broremote below): the link fails with LNK1201.
set_target_properties(broremote PROPERTIES COMPILE_PDB_NAME broremote_lib)
target_include_directories(broremote
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(broremote PUBLIC Threads::Threads)
target_compile_definitions(broremote PRIVATE BROREMOTE_VERSION_STRING="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(broremote PRIVATE ws2_32 advapi32)
    target_compile_definitions(broremote PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()
# ---- VA-API encoder (Linux) ------------------------------------------------------
# Auto: on when libva, libva-drm and libdrm (for the fourcc header) are found.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(BROREMOTE_VA IMPORTED_TARGET GLOBAL libva libva-drm libdrm)
    endif()
    if(BROREMOTE_VA_FOUND)
        set(_broremote_vaapi_default ON)
    else()
        set(_broremote_vaapi_default OFF)
    endif()
    option(BROREMOTE_WITH_VAAPI "Build the VA-API encoder" ${_broremote_vaapi_default})
    if(BROREMOTE_WITH_VAAPI AND NOT BROREMOTE_VA_FOUND)
        message(FATAL_ERROR "BROREMOTE_WITH_VAAPI is ON but libva, libva-drm and libdrm were not all found")
    endif()
    if(BROREMOTE_WITH_VAAPI)
        message(STATUS "broremote: VA-API encoder enabled (libva ${BROREMOTE_VA_libva_VERSION})")
        target_sources(broremote PRIVATE
            src/vaapi/va_util.cpp
            src/vaapi/va_device.cpp
            src/vaapi/va_source.cpp
            src/vaapi/va_vpp.cpp
            src/vaapi/va_encoder.cpp
            src/vaapi/va_h264.cpp
        )
        target_compile_definitions(broremote PRIVATE BROREMOTE_HAVE_VAAPI)
        target_link_libraries(broremote PRIVATE PkgConfig::BROREMOTE_VA)
    else()
        message(STATUS "broremote: VA-API encoder disabled (libva, libva-drm and libdrm not all found)")
    endif()
else()
    set(BROREMOTE_WITH_VAAPI OFF)
endif()

if(MSVC)
    target_compile_options(broremote PRIVATE /W4)
else()
    target_compile_options(broremote PRIVATE -Wall -Wextra)
endif()

# ---- the broremote executable: proxy and serve-test ----------------------------
if(BROREMOTE_BUILD_TOOLS)
    add_executable(broremote_cli tools/broremote.cpp tools/test_pattern.cpp)
    # ARCHIVE_OUTPUT_NAME: keep an import library MSVC may emit for the
    # executable from colliding with broremote.lib.
    set_target_properties(broremote_cli PROPERTIES OUTPUT_NAME broremote ARCHIVE_OUTPUT_NAME broremote_cli)
    target_link_libraries(broremote_cli PRIVATE broremote)
    if(WIN32)
        target_compile_definitions(broremote_cli PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
endif()

if(BROREMOTE_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR BRO_BUILD_TESTS))
    include(CTest)
    add_subdirectory(tests)
endif()
