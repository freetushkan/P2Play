# p2play_core: the core (static), C API in p2play.h
add_library(p2play_core STATIC
        api.cpp
        capi.cpp
        core.cpp
        http.cpp
        ram_cache.cpp
        store.cpp
        stream.cpp
)
target_include_directories(p2play_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(p2play_core PUBLIC torrent-rasterbar)
target_compile_definitions(p2play_core PRIVATE ${P2P_PLATFORM_DEFINES})
