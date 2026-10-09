cmake_minimum_required(VERSION 3.24)

project(broremote VERSION 0.1.0 LANGUAGES CXX)

# Dependencies (cmake/bro_deps.cmake): an outer project's target wins (under bro
# these are all bro's), else the ../<name> working tree beside the top-level
# project, else the pinned commit, fetched at configure.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bro_deps.cmake)

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
endif()

option(BROREMOTE_BUILD_TESTS "Build the broremote test suite" ON)
option(BROREMOTE_BUILD_TOOLS "Build the broremote command-line tool" ON)
# The JavaScript binding (src/api, broremote_api): off by default, so the
# library builds with nothing beside it; bro, which mounts it, turns it on.
# It needs bronze (and brass) checked out beside this repository.
option(BROREMOTE_ENABLE_API "Build the Bronze JavaScript API (bro.remote)" OFF)

# A top-level MSVC build links the CRT statically, so broremote.exe needs no
# VC++ runtime. With the binding on it is the DLL CRT instead: the binding
# talks to bronze's shared runtime DLL across a C++ boundary (strings,
# std::function), which only works with one CRT heap on both sides.
if(MSVC AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(BROREMOTE_ENABLE_API)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

find_package(Threads REQUIRED)

# ---- library -------------------------------------------------------------------
# The core (protocol, server, client) needs brolink, which carries it (the
# framing, the local listener and streams, the ssh proxy, lanes), and
# brovideo, which holds every codec (Raw, VA-API, Media Foundation) and
# decides at run time which of them this machine has.
set(BROREMOTE_SOURCES
    src/protocol.cpp
    src/paths_common.cpp
    src/server.cpp
    src/server_io.cpp
    src/server_encode.cpp
    src/client.cpp
    src/connect.cpp
    src/latency.cpp
    src/viewer.cpp
    src/proxy.cpp
    src/server_audio.cpp
    src/audio_client.cpp
    src/audio/audio.cpp
    src/audio/device_paced.cpp
)

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
    target_compile_definitions(broremote PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()
# ---- the audio lane's devices (src/audio) -----------------------------------------
# Platform APIs only: WASAPI on Windows (the viewer's mic and speakers),
# PipeWire on Linux (the host's monitor capture and virtual mic; needs
# libpipewire-0.3 at build time, else the host has no audio). Elsewhere, and
# in tests, the timer-paced endpoints.
set(BROREMOTE_HAS_PIPEWIRE OFF)
if(WIN32)
    target_sources(broremote PRIVATE src/audio/device_wasapi.cpp)
    target_link_libraries(broremote PRIVATE ole32 avrt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(BROREMOTE_PIPEWIRE "The host's audio through PipeWire" ON)
    if(BROREMOTE_PIPEWIRE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(BROREMOTE_LIBPIPEWIRE QUIET IMPORTED_TARGET GLOBAL libpipewire-0.3)
        endif()
        if(BROREMOTE_LIBPIPEWIRE_FOUND)
            set(BROREMOTE_HAS_PIPEWIRE ON)
            target_sources(broremote PRIVATE src/audio/device_pipewire.cpp)
            target_link_libraries(broremote PRIVATE PkgConfig::BROREMOTE_LIBPIPEWIRE)
            message(STATUS "broremote: host audio through PipeWire (libpipewire ${BROREMOTE_LIBPIPEWIRE_VERSION})")
        else()
            message(STATUS "broremote: libpipewire-0.3 not found, the host has no audio lane devices")
        endif()
    endif()
endif()
target_compile_definitions(broremote PRIVATE BROREMOTE_HAS_PIPEWIRE=$<BOOL:${BROREMOTE_HAS_PIPEWIRE}>)
# ---- brolink: the transport ---------------------------------------------------------
# A bro_dependency (cmake/bro_deps.cmake): an existing brolink target (bro, or
# bromux beside it, added it), else ../brolink, else the pinned commit.
bro_dependency(brolink GITHUB wlejon/brolink
               OPTIONS BROLINK_BUILD_TESTS=OFF)
target_link_libraries(broremote PUBLIC brolink::brolink)
# ---- brovideo: the codecs ----------------------------------------------------------
# An existing brovideo target (bro builds it), else ../brovideo, else the pinned
# commit. Its backends are its own options (BROVIDEO_WITH_VAAPI,
# BROVIDEO_WITH_MF); on Linux it brings brodmabuf.
bro_dependency(brovideo GITHUB wlejon/brovideo
               OPTIONS BROVIDEO_BUILD_TESTS=OFF)
target_link_libraries(broremote PUBLIC brovideo::brovideo)

if(MSVC)
    target_compile_options(broremote PRIVATE /W4)
else()
    target_compile_options(broremote PRIVATE -Wall -Wextra)
endif()

# ---- the broremote executable: proxy, serve-test, probe, ... ------------------
# No window anywhere: the viewers with a display are bro's (<remoteview>).
if(BROREMOTE_BUILD_TOOLS)
    add_executable(broremote_cli tools/broremote.cpp tools/connect.cpp tools/test_pattern.cpp
                                 tools/probe.cpp tools/png.cpp)
    # ARCHIVE_OUTPUT_NAME: keep an import library MSVC may emit for the
    # executable from colliding with broremote.lib.
    set_target_properties(broremote_cli PROPERTIES OUTPUT_NAME broremote ARCHIVE_OUTPUT_NAME broremote_cli)
    target_link_libraries(broremote_cli PRIVATE broremote)
    if(WIN32)
        target_compile_definitions(broremote_cli PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
endif()

# ---- the JavaScript binding (broremote_api) -----------------------------------------
# bronze, the JavaScript runtime the binding is written against (it brings its
# code generator, brass): an existing bronze_runtime_shared target (bro's), else
# ../bronze, else the pinned commit. The binding crosses bronze's C++ embed
# boundary, so it has to build with the bronze (and the compiler and CRT) of
# whatever it is linked into.
if(BROREMOTE_ENABLE_API)
    bro_dependency(bronze GITHUB wlejon/bronze TARGET bronze_runtime_shared
                   OPTIONS BRONZE_BUILD_SHARED_RUNTIME=ON BRONZE_BUILD_TESTS=OFF)
    message(STATUS "broremote: JavaScript binding (broremote_api) enabled")
    add_subdirectory(src/api)
endif()

if(BROREMOTE_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR BRO_BUILD_TESTS))
    include(CTest)
    add_subdirectory(tests)
endif()
