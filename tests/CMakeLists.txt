enable_testing()

# Each test is a plain executable whose exit status is non-zero on any failed
# check (check.h); a watchdog in each turns a hang into a failure.
#   broremote_test(<name> [sources...] [ARGS <test arguments...>] [TIMEOUT s])
function(broremote_test name)
    cmake_parse_arguments(PARSE_ARGV 1 BT "" "TIMEOUT" "ARGS")
    add_executable(${name} ${name}.cpp ${BT_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE broremote)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tools)
    if(WIN32)
        target_compile_definitions(${name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME ${name} COMMAND ${name} ${BT_ARGS})
    if(NOT BT_TIMEOUT)
        set(BT_TIMEOUT 300)
    endif()
    set_tests_properties(${name} PROPERTIES TIMEOUT ${BT_TIMEOUT})
endfunction()

broremote_test(test_wire)
broremote_test(test_server)
# Remoting through a hardware encoder, against ffmpeg (Linux; skipped, exit
# 77, without one). The encoders themselves are tested in brovideo.
if(UNIX AND NOT APPLE)
    if(TARGET broremote_cli)
        broremote_test(test_hw_stream ${PROJECT_SOURCE_DIR}/tools/test_pattern.cpp
                       ARGS $<TARGET_FILE:broremote_cli> TIMEOUT 900)
        add_dependencies(test_hw_stream broremote_cli)
    else()
        broremote_test(test_hw_stream ${PROJECT_SOURCE_DIR}/tools/test_pattern.cpp TIMEOUT 900)
    endif()
    set_tests_properties(test_hw_stream PROPERTIES SKIP_RETURN_CODE 77)
endif()

# broremote-view's input path through the real Viewer (SDL's offscreen driver).
if(TARGET broremote_view_core)
    broremote_test(test_view TIMEOUT 120)
    target_link_libraries(test_view PRIVATE broremote_view_core)
    broremote_copy_runtime_dlls(test_view)
endif()

# The JavaScript binding in a bronze realm (needs ../bronze and ../brass).
# bronze builds its shared runtime into its own directory; on Windows the
# DLL is copied beside the test, where the loader looks.
if(TARGET broremote_api)
    add_executable(broremote_test_api test_api.cpp)
    target_link_libraries(broremote_test_api PRIVATE broremote_api broremote bronze_runtime_shared)
    target_include_directories(broremote_test_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(WIN32)
        target_compile_definitions(broremote_test_api PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        add_custom_command(TARGET broremote_test_api POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared> $<TARGET_FILE_DIR:broremote_test_api>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared> $<TARGET_FILE_DIR:broremote_test_api>)
    endif()
    if(NOT BRASS_HOST_BACKEND)
        message(STATUS "broremote: broremote_test_api not registered (brass has no code generator here)")
    else()
        add_test(NAME broremote_test_api COMMAND broremote_test_api)
        set_tests_properties(broremote_test_api PROPERTIES TIMEOUT 180)
        if(WIN32)
            set_tests_properties(broremote_test_api PROPERTIES
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:broremote_test_api>;$ENV{PATH}")
        endif()
    endif()
endif()

if(TARGET broremote_cli)
    broremote_test(test_proxy ${PROJECT_SOURCE_DIR}/tools/test_pattern.cpp ARGS $<TARGET_FILE:broremote_cli>)
    add_dependencies(test_proxy broremote_cli)
endif()
