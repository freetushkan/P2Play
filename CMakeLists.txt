cmake_minimum_required(VERSION 3.16)

set(PLATFORM_LINUX OFF CACHE BOOL "Linux target")
set(PLATFORM_SWITCH OFF CACHE BOOL "Nintendo Switch target")
set(PLATFORM_PS4 OFF CACHE BOOL "Sony PS4 target")
set(PLATFORM_PS5 OFF CACHE BOOL "Sony PS5 target")
set(PLATFORM_ANDROID OFF CACHE BOOL "Android target (NDK, adb shell executable)")
set(BOOST_INCLUDE_DIR "" CACHE PATH "Boost headers (ci/boost-headers.sh)")

include(cmake/toolchain.cmake)

project(p2play C CXX)

set(VERSION_MAJOR "0")
set(VERSION_MINOR "1")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if (NOT EXISTS "${BOOST_INCLUDE_DIR}/boost/version.hpp")
    message(FATAL_ERROR "BOOST_INCLUDE_DIR must point to the Boost headers (ci/boost-headers.sh)")
endif ()
set(P2P_BOOST_INCLUDE_DIR ${BOOST_INCLUDE_DIR})

# Switch and PS4: asio wakes its reactor through a loopback socket pair (no pipe/socketpair
# in libnx), the SDK header gaps are filled by compat/
if (PLATFORM_SWITCH OR PLATFORM_PS4)
    file(READ "${BOOST_INCLUDE_DIR}/boost/asio/detail/select_interrupter.hpp" _p2p_si)
    if (NOT _p2p_si MATCHES "BOOST_ASIO_USE_SOCKET_SELECT_INTERRUPTER")
        message(FATAL_ERROR "Boost headers without patches/boost: rerun ci/boost-headers.sh")
    endif ()
    add_compile_options("SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/p2p_platform.h")
    add_compile_definitions(BOOST_ASIO_USE_SOCKET_SELECT_INTERRUPTER)
    set(TORRENT_NO_TRY_SIGNAL ON)
endif ()
if (PLATFORM_SWITCH)
    include_directories(BEFORE SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/compat/switch)
    add_compile_definitions(BOOST_ASIO_DISABLE_LOCAL_SOCKETS BOOST_ASIO_USE_NULL_SIGNAL_BLOCKER)
elseif (PLATFORM_PS4)
    add_compile_definitions(BOOST_ASIO_DISABLE_KQUEUE)
endif ()
set(Boost_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/boost)
set(CMAKE_POLICY_DEFAULT_CMP0167 NEW)

# consoles: debug info everywhere, for crash log addresses (the released binaries are stripped)
if (PLATFORM_SWITCH OR PLATFORM_PS4 OR PLATFORM_PS5)
    add_compile_options(-g)
endif ()

# PS4: our link script instead of the toolchain's one (cmake/ps4-link.x says why)
if (PLATFORM_PS4)
    include(cmake/ps4-link.cmake)
endif ()

include(cmake/libtorrent.cmake)

add_subdirectory(src/core)
add_subdirectory(src/app)
