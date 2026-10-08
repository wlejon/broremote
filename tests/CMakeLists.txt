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
# The VA-API encoder against ffmpeg (skipped, exit 77, without a VA encoder).
# GBM supplies the dmabuf frames.
if(BROREMOTE_WITH_VAAPI)
    pkg_check_modules(BROREMOTE_GBM IMPORTED_TARGET gbm)
    if(BROREMOTE_GBM_FOUND)
        broremote_test(test_vaapi ${PROJECT_SOURCE_DIR}/tools/test_pattern.cpp TIMEOUT 900)
        target_link_libraries(test_vaapi PRIVATE PkgConfig::BROREMOTE_GBM PkgConfig::BROREMOTE_VA)
        set_tests_properties(test_vaapi PROPERTIES SKIP_RETURN_CODE 77)
    else()
        message(STATUS "broremote: test_vaapi not built (gbm not found)")
    endif()
endif()

if(TARGET broremote_cli)
    broremote_test(test_proxy ${PROJECT_SOURCE_DIR}/tools/test_pattern.cpp ARGS $<TARGET_FILE:broremote_cli>)
    add_dependencies(test_proxy broremote_cli)
endif()
