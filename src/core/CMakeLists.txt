# p2play_core: the core (static), C API in p2play.h

# the web interface, built in: the Silo plugin's own pages (plugins/mediamesh), one source for both
set(P2P_UI_DIR ${PROJECT_SOURCE_DIR}/plugins/mediamesh)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ui_files.cpp
        COMMAND ${CMAKE_COMMAND} -DUI_DIR=${P2P_UI_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ui_files.cpp
                -P ${PROJECT_SOURCE_DIR}/cmake/embed-ui.cmake
        DEPENDS ${P2P_UI_DIR}/index.html ${P2P_UI_DIR}/app.js ${P2P_UI_DIR}/style.css ${P2P_UI_DIR}/lang/en.json ${P2P_UI_DIR}/lang/ru.json
                ${PROJECT_SOURCE_DIR}/cmake/embed-ui.cmake)

add_library(p2play_core STATIC
        ${CMAKE_CURRENT_BINARY_DIR}/ui_files.cpp
        api_auth.cpp api_routes.cpp api_p2play.cpp api_catalog.cpp mesh.cpp mesh_net.cpp mesh_ext.cpp hybrid_disk.cpp
        torrs.cpp
        auth.cpp
        capi.cpp
        core.cpp
        covers.cpp
        http.cpp
        ram_cache.cpp
        store.cpp
        stream.cpp
)
target_include_directories(p2play_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(p2play_core PUBLIC torrent-rasterbar)
target_compile_definitions(p2play_core PRIVATE ${P2P_PLATFORM_DEFINES}
        "P2P_NATIVE_VERSION=\"${VERSION_MAJOR}.${VERSION_MINOR}\""
)

if (PLATFORM_LINUX AND NOT P2P_EMBEDDED_ONLY)
    target_compile_definitions(p2play_core PRIVATE P2P_FETCH_TEST=1)
    add_executable(p2play_fetch_test ${PROJECT_SOURCE_DIR}/tools/http-fetch-test.cpp)
    target_compile_definitions(p2play_fetch_test PRIVATE P2P_FETCH_TEST=1)
    target_link_libraries(p2play_fetch_test PRIVATE p2play_core)

    add_executable(p2play_http_bind_test ${PROJECT_SOURCE_DIR}/tools/http-bind-test.cpp)
    target_link_libraries(p2play_http_bind_test PRIVATE p2play_core)
endif ()
