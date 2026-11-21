cmake_minimum_required(VERSION 3.16)

#
# p2spike PS5 package: a PS4 "gd" package with the launcher (main.c) and ps5/p2spike.elf.
# Built apart from P2Play, with the pacbrew OpenOrbis toolchain:
#   cmake -S src/spike/ps5/launcher -B build-ps5pkg \
#         -DCMAKE_TOOLCHAIN_FILE=/opt/pacbrew/ps4/openorbis/cmake/ps4.cmake \
#         -DP2SPIKE_ELF=<the PS5 build>/p2spike.elf
#   cmake --build build-ps5pkg
#

project(p2spike_ps5 C)

set(P2SPIKE_ELF "" CACHE FILEPATH "p2spike.elf of the PS5 build")
set(PKG_TITLE_ID "PTWO00005" CACHE STRING "package title id")
set(PKG_VERSION "01.00" CACHE STRING "package version (xx.yy)")
set(P2PLAY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)

if (NOT EXISTS "${P2SPIKE_ELF}")
    message(FATAL_ERROR "P2SPIKE_ELF must point to the PS5 build of p2spike (p2spike.elf)")
endif ()

# keeps .data.* in the loaded data segment (see cmake/ps4-link.x)
include(${P2PLAY_ROOT}/cmake/ps4-link.cmake)

set(PKG_DIR ${CMAKE_CURRENT_BINARY_DIR}/pkgroot)
file(REMOVE_RECURSE ${PKG_DIR})
file(MAKE_DIRECTORY ${PKG_DIR}/ps5)
file(COPY ${P2PLAY_ROOT}/data/ps4/sce_sys ${P2PLAY_ROOT}/data/ps4/sce_module DESTINATION ${PKG_DIR})
configure_file(${P2SPIKE_ELF} ${PKG_DIR}/ps5/p2spike.elf COPYONLY)

add_executable(p2spike_launcher main.c)
target_compile_options(p2spike_launcher PRIVATE -Wall -Wextra)
target_link_libraries(p2spike_launcher SceSystemService SceSysmodule SceNet)

add_self(p2spike_launcher)
# category "gd": the PS5 refuses "gde" packages
add_pkg(p2spike_launcher "${PKG_DIR}" ${PKG_TITLE_ID} "p2spike PS5" ${PKG_VERSION} 0 gd)
