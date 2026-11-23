# p2play_core: the core (static), C API in p2play.h
# compatibility layers: each can be left out
option(P2P_COMPAT_MATRIX "The TorrServer MatriX API and its Basic authentication" ON)

# the web interface, built in: the Silo plugin's own pages (plugins/mediamesh), one source for both
set(P2P_UI_DIR ${PROJECT_SOURCE_DIR}/plugins/mediamesh)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ui_files.cpp
        COMMAND ${CMAKE_COMMAND} -DUI_DIR=${P2P_UI_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ui_files.cpp
                -P ${PROJECT_SOURCE_DIR}/cmake/embed-ui.cmake
        DEPENDS ${P2P_UI_DIR}/index.html ${P2P_UI_DIR}/app.js ${P2P_UI_DIR}/style.css
                ${PROJECT_SOURCE_DIR}/cmake/embed-ui.cmake)

add_library(p2play_core STATIC
        ${CMAKE_CURRENT_BINARY_DIR}/ui_files.cpp
        api_auth.cpp api_silo.cpp api_catalog.cpp mesh.cpp mesh_net.cpp mesh_ext.cpp hybrid_disk.cpp
        api_matrix.cpp torrs.cpp
        auth.cpp
        capi.cpp
        core.cpp
        http.cpp
        ram_cache.cpp
        store.cpp
        stream.cpp
)
target_include_directories(p2play_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(p2play_core PUBLIC torrent-rasterbar)
target_compile_definitions(p2play_core PRIVATE ${P2P_PLATFORM_DEFINES}
        "P2P_NATIVE_VERSION=\"${VERSION_MAJOR}.${VERSION_MINOR}\""
        P2P_COMPAT_MATRIX=$<IF:$<BOOL:${P2P_COMPAT_MATRIX}>,1,0>)
