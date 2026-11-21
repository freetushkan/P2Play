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

# 0 in ${out} when the working tree is the pinned tag with ${ARGN} applied
function(p2p_lt_tree_is out)
    set(index ${CMAKE_BINARY_DIR}/libtorrent-patched.index)
    file(REMOVE ${index})
    execute_process(COMMAND ${CMAKE_COMMAND} -E env GIT_INDEX_FILE=${index} git read-tree HEAD
            WORKING_DIRECTORY ${P2P_LT_DIR} RESULT_VARIABLE r OUTPUT_QUIET ERROR_QUIET)
    if (r EQUAL 0 AND ARGN)
        execute_process(COMMAND ${CMAKE_COMMAND} -E env GIT_INDEX_FILE=${index} git apply --cached ${ARGN}
                WORKING_DIRECTORY ${P2P_LT_DIR} RESULT_VARIABLE r OUTPUT_QUIET ERROR_QUIET)
    endif ()
    if (NOT r EQUAL 0)
        set(${out} -1 PARENT_SCOPE)
        file(REMOVE ${index})
        return()
    endif ()
    execute_process(COMMAND ${CMAKE_COMMAND} -E env GIT_INDEX_FILE=${index} git diff --quiet
            WORKING_DIRECTORY ${P2P_LT_DIR} RESULT_VARIABLE r OUTPUT_QUIET ERROR_QUIET)
    file(REMOVE ${index})
    set(${out} ${r} PARENT_SCOPE)
endfunction()

# the hash of the working tree's changes: tracked files and the names of new ones
function(p2p_lt_tree_hash out)
    execute_process(COMMAND git diff HEAD WORKING_DIRECTORY ${P2P_LT_DIR} OUTPUT_VARIABLE diff ERROR_QUIET)
    execute_process(COMMAND git ls-files --others --exclude-standard WORKING_DIRECTORY ${P2P_LT_DIR}
            OUTPUT_VARIABLE others ERROR_QUIET)
    string(SHA256 hash "${diff}${others}")
    set(${out} ${hash} PARENT_SCOPE)
endfunction()

# the tree configure patched last (any build directory): replaced freely
execute_process(COMMAND git rev-parse --absolute-git-dir WORKING_DIRECTORY ${P2P_LT_DIR}
        OUTPUT_VARIABLE P2P_LT_GIT_DIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(P2P_LT_STAMP ${P2P_LT_GIT_DIR}/p2play-patched)

if (P2P_LT_PATCHES)
    p2p_lt_tree_is(applied ${P2P_LT_PATCHES})
    if (applied EQUAL -1)
        message(FATAL_ERROR "libtorrent: patches/libtorrent do not apply to the pinned libtorrent")
    endif ()
    if (NOT applied EQUAL 0)
        # the working tree: clean, patched by configure before (an earlier series), or the
        # first patches of the series; else changed by hand
        set(known OFF)
        p2p_lt_tree_hash(current)
        if (EXISTS ${P2P_LT_STAMP})
            file(READ ${P2P_LT_STAMP} stamp)
            if (stamp STREQUAL current)
                set(known ON)
            endif ()
        endif ()
        set(prefix ${P2P_LT_PATCHES})
        while (NOT known)
            p2p_lt_tree_is(same ${prefix})
            if (same EQUAL 0)
                set(known ON)
                break()
            endif ()
            list(LENGTH prefix n)
            if (n EQUAL 0)
                break()
            endif ()
            list(REMOVE_AT prefix -1)
        endwhile ()
        if (NOT known)
            message(FATAL_ERROR "libtorrent: third_party/libtorrent has changes outside patches/libtorrent, "
                    "reset it with: git -C third_party/libtorrent checkout -- .")
        endif ()
        # back to the tag: the tracked files, and the files the patches create
        execute_process(COMMAND git checkout -- . WORKING_DIRECTORY ${P2P_LT_DIR})
        foreach (patch ${P2P_LT_PATCHES})
            # the file headers only: "--- /dev/null" then "+++ b/<new file>"
            file(STRINGS ${patch} headers REGEX "^(---|\\+\\+\\+) ")
            set(prev "")
            foreach (line ${headers})
                if (prev STREQUAL "--- /dev/null" AND line MATCHES "^\\+\\+\\+ b/(.*)$")
                    file(REMOVE ${P2P_LT_DIR}/${CMAKE_MATCH_1})
                endif ()
                set(prev "${line}")
            endforeach ()
        endforeach ()
        execute_process(COMMAND git apply ${P2P_LT_PATCHES} WORKING_DIRECTORY ${P2P_LT_DIR} RESULT_VARIABLE result)
        if (NOT result EQUAL 0)
            message(FATAL_ERROR "libtorrent: cannot apply patches/libtorrent")
        endif ()
        message(STATUS "libtorrent: patches/libtorrent applied")
    endif ()
    p2p_lt_tree_hash(current)
    file(WRITE ${P2P_LT_STAMP} ${current})
endif ()

add_subdirectory(${P2P_LT_DIR} EXCLUDE_FROM_ALL)

if (P2P_PLATFORM_DEFINES)
    target_compile_definitions(torrent-rasterbar PUBLIC ${P2P_PLATFORM_DEFINES})
endif ()
