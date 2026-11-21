# libtorrent from third_party/libtorrent, static, without external crypto libraries
# (built-in SHA-1/SHA-256, no TLS trackers or SSL torrents)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(deprecated-functions OFF CACHE BOOL "" FORCE)
set(i2p OFF CACHE BOOL "" FORCE)
set(encryption ON CACHE BOOL "" FORCE)
set(dht ON CACHE BOOL "" FORCE)
set(logging ON CACHE BOOL "" FORCE)
set(build_tests OFF CACHE BOOL "" FORCE)
set(build_examples OFF CACHE BOOL "" FORCE)
set(build_tools OFF CACHE BOOL "" FORCE)
set(python-bindings OFF CACHE BOOL "" FORCE)

set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL ON)
set(CMAKE_DISABLE_FIND_PACKAGE_GnuTLS ON)
set(CMAKE_DISABLE_FIND_PACKAGE_LibGcrypt ON)

# patches/libtorrent/*.patch on top of the pinned upstream tag, once
set(P2P_LT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libtorrent)
file(GLOB P2P_LT_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/libtorrent/*.patch)
list(SORT P2P_LT_PATCHES)
if (P2P_LT_PATCHES)
    # the patched tree built in a scratch index, compared with the working tree
    set(P2P_LT_INDEX ${CMAKE_BINARY_DIR}/libtorrent-patched.index)
    file(REMOVE ${P2P_LT_INDEX})
    execute_process(COMMAND ${CMAKE_COMMAND} -E env GIT_INDEX_FILE=${P2P_LT_INDEX} git read-tree HEAD
            WORKING_DIRECTORY ${P2P_LT_DIR} RESULT_VARIABLE r1 OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND ${CMAKE_COMMAND} -E env GIT_INDEX_FILE=${P2P_LT_INDEX} git apply --cached ${P2P_LT_PATCHES}
            WORKING_DIRECTORY ${P2P_LT_DIR} RESULT_VARIABLE r2 OUTPUT_QUIET ERROR_QUIET)
    if (NOT r1 EQUAL 0 OR NOT r2 EQUAL 0)
        message(FATAL_ERROR "libtorrent: patches/libtorrent do not apply to the pinned libtorrent")
    endif ()
    execute_process(COMMAND ${CMAKE_COMMAND} -E env GIT_INDEX_FILE=${P2P_LT_INDEX} git diff --quiet
            WORKING_DIRECTORY ${P2P_LT_DIR} RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
    file(REMOVE ${P2P_LT_INDEX})
    if (NOT applied EQUAL 0)
        execute_process(COMMAND git diff --quiet WORKING_DIRECTORY ${P2P_LT_DIR} RESULT_VARIABLE dirty)
        if (NOT dirty EQUAL 0)
            message(FATAL_ERROR "libtorrent: third_party/libtorrent has changes outside patches/libtorrent, "
                    "reset it with: git -C third_party/libtorrent checkout -- .")
        endif ()
        execute_process(COMMAND git apply ${P2P_LT_PATCHES} WORKING_DIRECTORY ${P2P_LT_DIR} RESULT_VARIABLE result)
        if (NOT result EQUAL 0)
            message(FATAL_ERROR "libtorrent: cannot apply patches/libtorrent")
        endif ()
        message(STATUS "libtorrent: patches/libtorrent applied")
    endif ()
endif ()

add_subdirectory(${P2P_LT_DIR} EXCLUDE_FROM_ALL)

if (P2P_PLATFORM_DEFINES)
    target_compile_definitions(torrent-rasterbar PUBLIC ${P2P_PLATFORM_DEFINES})
endif ()
