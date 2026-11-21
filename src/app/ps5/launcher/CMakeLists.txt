cmake_minimum_required(VERSION 3.16)

#
# P2Play PS5 package: a PS4 "gd" package with the launcher (main.c) and ps5/p2play.elf.
# Built apart from P2Play, with the pacbrew OpenOrbis toolchain:
#   cmake -S src/app/ps5/launcher -B build-ps5pkg \
#         -DCMAKE_TOOLCHAIN_FILE=/opt/pacbrew/ps4/openorbis/cmake/ps4.cmake \
#         -DP2PLAY_ELF=<the PS5 build>/p2play.elf
#   cmake --build build-ps5pkg
#

project(p2play_ps5 C)

set(P2PLAY_ELF "" CACHE FILEPATH "p2play.elf of the PS5 build")
set(PKG_TITLE_ID "PTWO00005" CACHE STRING "package title id")
set(PKG_VERSION "01.00" CACHE STRING "package version (xx.yy)")
set(P2PLAY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)

if (NOT EXISTS "${P2PLAY_ELF}")
    message(FATAL_ERROR "P2PLAY_ELF must point to the PS5 build of p2play (p2play.elf)")
endif ()

# keeps .data.* in the loaded data segment (see cmake/ps4-link.x)
include(${P2PLAY_ROOT}/cmake/ps4-link.cmake)

set(PKG_DIR ${CMAKE_CURRENT_BINARY_DIR}/pkgroot)
file(REMOVE_RECURSE ${PKG_DIR})
file(MAKE_DIRECTORY ${PKG_DIR}/ps5)
file(COPY ${P2PLAY_ROOT}/data/ps4/sce_sys ${P2PLAY_ROOT}/data/ps4/sce_module DESTINATION ${PKG_DIR})
configure_file(${P2PLAY_ELF} ${PKG_DIR}/ps5/p2play.elf COPYONLY)

add_executable(p2play_launcher main.c)
target_compile_options(p2play_launcher PRIVATE -Wall -Wextra)
target_link_libraries(p2play_launcher SceSystemService SceSysmodule SceNet)

add_self(p2play_launcher)
# category "gd": the PS5 refuses "gde" packages
add_pkg(p2play_launcher "${PKG_DIR}" ${PKG_TITLE_ID} "P2Play PS5" ${PKG_VERSION} 0 gd)
