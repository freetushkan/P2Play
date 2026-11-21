cmake_minimum_required(VERSION 3.16)

set(PLATFORM_LINUX OFF CACHE BOOL "Linux target")
set(PLATFORM_SWITCH OFF CACHE BOOL "Nintendo Switch target")
set(PLATFORM_PS4 OFF CACHE BOOL "Sony PS4 target")
set(PLATFORM_PS5 OFF CACHE BOOL "Sony PS5 target")
set(BOOST_INCLUDE_DIR "" CACHE PATH "Boost headers (ci/boost-headers.sh)")

include(cmake/toolchain.cmake)

project(p2play C CXX)

set(VERSION_MAJOR "0")
set(VERSION_MINOR "1")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT EXISTS "${BOOST_INCLUDE_DIR}/boost/version.hpp")
    message(FATAL_ERROR "BOOST_INCLUDE_DIR must point to the Boost headers (ci/boost-headers.sh)")
endif ()
set(P2P_BOOST_INCLUDE_DIR ${BOOST_INCLUDE_DIR})
set(Boost_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/boost)
set(CMAKE_POLICY_DEFAULT_CMP0167 NEW)

include(cmake/libtorrent.cmake)

add_subdirectory(src/spike)
