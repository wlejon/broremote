# broremote_api: bro.remote, the binding a bro host mounts (bro's
# installSiblingApis). It links the library and bronze's shared runtime and
# names nothing of bro: the host reaches it through the hooks in api.h.
add_library(broremote_api STATIC
    api.cpp
    viewer_api.cpp
)
set_target_properties(broremote_api PROPERTIES COMPILE_PDB_NAME broremote_api)
target_include_directories(broremote_api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(broremote_api PUBLIC broremote bronze_runtime_shared)
target_compile_features(broremote_api PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(broremote_api PRIVATE /W4)
else()
    target_compile_options(broremote_api PRIVATE -Wall -Wextra)
endif()
