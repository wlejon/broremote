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
    src/proxy.cpp
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
# ---- brolink: the transport ---------------------------------------------------------
# An existing brolink target (bro, or bromux beside it, added it), else
# -DBROLINK_DIR, else a checkout beside this one (../brolink).
if(NOT TARGET brolink)
    if(DEFINED BROLINK_DIR AND EXISTS "${BROLINK_DIR}/CMakeLists.txt")
        # Explicit BROLINK_DIR supplied
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../brolink/CMakeLists.txt")
        set(BROLINK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../brolink" CACHE PATH "brolink source tree")
    endif()
    if(NOT BROLINK_DIR OR NOT EXISTS "${BROLINK_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "brolink not found: check it out beside this repository (../brolink) or pass -DBROLINK_DIR=<path>.")
    endif()
    set(BROLINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${BROLINK_DIR}" "${CMAKE_BINARY_DIR}/brolink" EXCLUDE_FROM_ALL)
endif()
target_link_libraries(broremote PUBLIC brolink::brolink)
# ---- brovideo: the codecs ----------------------------------------------------------
# An existing brovideo target (bro builds it), else -DBROVIDEO_DIR, else a
# checkout beside this one (../brovideo). Its backends are its own options
# (BROVIDEO_WITH_VAAPI, BROVIDEO_WITH_MF); on Linux it needs ../brodmabuf.
if(NOT TARGET brovideo)
    if(DEFINED BROVIDEO_DIR AND EXISTS "${BROVIDEO_DIR}/CMakeLists.txt")
        # Explicit BROVIDEO_DIR supplied
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../brovideo/CMakeLists.txt")
        set(BROVIDEO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../brovideo" CACHE PATH "brovideo source tree")
    endif()
    if(NOT BROVIDEO_DIR OR NOT EXISTS "${BROVIDEO_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "brovideo not found: check it out beside this repository (../brovideo) or pass -DBROVIDEO_DIR=<path>.")
    endif()
    set(BROVIDEO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${BROVIDEO_DIR}" "${CMAKE_BINARY_DIR}/brovideo" EXCLUDE_FROM_ALL)
endif()
target_link_libraries(broremote PUBLIC brovideo::brovideo)

if(MSVC)
    target_compile_options(broremote PRIVATE /W4)
else()
    target_compile_options(broremote PRIVATE -Wall -Wextra)
endif()

# ---- the broremote executable: proxy and serve-test ----------------------------
if(BROREMOTE_BUILD_TOOLS)
    add_executable(broremote_cli tools/broremote.cpp tools/connect.cpp tools/test_pattern.cpp)
    # ARCHIVE_OUTPUT_NAME: keep an import library MSVC may emit for the
    # executable from colliding with broremote.lib.
    set_target_properties(broremote_cli PROPERTIES OUTPUT_NAME broremote ARCHIVE_OUTPUT_NAME broremote_cli)
    target_link_libraries(broremote_cli PRIVATE broremote)
    if(WIN32)
        target_compile_definitions(broremote_cli PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
endif()

# ---- broremote-view (SDL3) --------------------------------------------------------
# SDL3: an existing SDL3::SDL3 target (bro builds its own, static), else an
# installed package. On Windows a vcpkg tree is looked for where it usually
# lives when no toolchain file points at it ($VCPKG_ROOT, ../vcpkg,
# ../../vcpkg; the x64-windows triplet), and SDL3.dll is copied next to the
# executables that need it.
#
# The CRT: a top-level MSVC build links the CRT statically (above), while
# vcpkg's x64-windows SDL3 is a DLL with its own (dynamic) CRT. That is
# sound: SDL's API never hands CRT objects across the DLL boundary (memory
# it allocates is freed with SDL_free), so each side keeps its own CRT and
# nothing here needs /MD. Inside bro, SDL3::SDL3 is bro's static SDL built
# with bro's CRT settings, which broremote then inherits, so nothing mixes.
if(NOT TARGET SDL3::SDL3)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND AND WIN32)
        set(_broremote_sdl_hints)
        foreach(_root "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg"
                      "${CMAKE_CURRENT_SOURCE_DIR}/../../vcpkg")
            if(_root AND EXISTS "${_root}/installed/x64-windows/share/sdl3")
                list(APPEND _broremote_sdl_hints "${_root}/installed/x64-windows")
            endif()
        endforeach()
        if(_broremote_sdl_hints)
            find_package(SDL3 CONFIG QUIET PATHS ${_broremote_sdl_hints} NO_DEFAULT_PATH)
        endif()
    endif()
endif()
if(TARGET SDL3::SDL3)
    set(_broremote_viewer_default ON)
else()
    set(_broremote_viewer_default OFF)
endif()
option(BROREMOTE_BUILD_VIEWER "Build broremote-view (needs SDL3)" ${_broremote_viewer_default})
if(BROREMOTE_BUILD_VIEWER AND NOT TARGET SDL3::SDL3)
    message(FATAL_ERROR "BROREMOTE_BUILD_VIEWER is ON but SDL3 was not found")
endif()

# Copy the DLLs `target` loads (SDL3.dll from a shared SDL) next to it.
function(broremote_copy_runtime_dlls target)
    if(WIN32 AND TARGET SDL3::SDL3-shared AND NOT TARGET SDL3::SDL3-static)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()

if(BROREMOTE_BUILD_VIEWER)
    message(STATUS "broremote: broremote-view enabled (SDL3)")
    # Everything but main(), so the tests drive the same code.
    add_library(broremote_view_core STATIC
        tools/view/keymap.cpp
        tools/view/input_map.cpp
        tools/view/session.cpp
        tools/view/latency.cpp
        tools/view/display_timing.cpp
        tools/view/viewer_app.cpp
        tools/view/png.cpp
        tools/connect.cpp
        tools/test_pattern.cpp
    )
    set_target_properties(broremote_view_core PROPERTIES COMPILE_PDB_NAME broremote_view_core)
    target_include_directories(broremote_view_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools/view
                                                          ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    target_link_libraries(broremote_view_core PUBLIC broremote SDL3::SDL3)
    if(WIN32)
        target_compile_definitions(broremote_view_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_executable(broremote_view tools/view/main.cpp)
    set_target_properties(broremote_view PROPERTIES OUTPUT_NAME broremote-view)
    target_link_libraries(broremote_view PRIVATE broremote_view_core)
    broremote_copy_runtime_dlls(broremote_view)
else()
    message(STATUS "broremote: broremote-view disabled (SDL3 not found)")
endif()

# ---- the JavaScript binding (broremote_api) -----------------------------------------
# bronze, the JavaScript runtime the binding is written against: an existing
# bronze_runtime_shared target (bro's), else a checkout beside this one
# (../bronze, with ../brass beside it, which bronze resolves itself), or an
# explicit -DBRONZE_DIR. No vendored fallback: the binding crosses bronze's C++
# embed boundary, so it has to build with the bronze (and the compiler and
# CRT) of whatever it is linked into.
if(BROREMOTE_ENABLE_API)
    if(NOT TARGET bronze_runtime_shared)
        set(BRONZE_BUILD_SHARED_RUNTIME ON CACHE BOOL "" FORCE)
        set(BRONZE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        if(DEFINED BRONZE_DIR AND EXISTS "${BRONZE_DIR}/CMakeLists.txt")
            # Explicit BRONZE_DIR supplied
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../bronze/CMakeLists.txt")
            set(BRONZE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bronze" CACHE PATH "bronze source tree")
        elseif(EXISTS "${CMAKE_SOURCE_DIR}/../bronze/CMakeLists.txt")
            set(BRONZE_DIR "${CMAKE_SOURCE_DIR}/../bronze" CACHE PATH "bronze source tree")
        endif()
        if(NOT BRONZE_DIR OR NOT EXISTS "${BRONZE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "bronze not found: check it out beside this repository (../bronze, with ../brass beside it) "
                "or pass -DBRONZE_DIR=<path>, or configure with -DBROREMOTE_ENABLE_API=OFF.")
        endif()
        add_subdirectory("${BRONZE_DIR}" "${CMAKE_BINARY_DIR}/bronze-build" EXCLUDE_FROM_ALL)
    endif()
    message(STATUS "broremote: JavaScript binding (broremote_api) enabled")
    add_subdirectory(src/api)
endif()

if(BROREMOTE_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR BRO_BUILD_TESTS))
    include(CTest)
    add_subdirectory(tests)
endif()
