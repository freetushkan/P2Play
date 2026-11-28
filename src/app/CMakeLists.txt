# P2Play, the application around the core: p2playd on Linux and Android, a homebrew
# application on Switch, PS4 and PS5
set(APP_SRC
        main.cpp
        platform.cpp
        https.cpp
)

# the crash handlers of pPlay, a file a platform
if (PLATFORM_PS4)
    list(APPEND APP_SRC ps4/ps4_compat.c ps4/crashlog.c ps4/crashlog_api.cpp
            screen/console.cpp screen/font.cpp screen/videoout_ps4.cpp)
elseif (PLATFORM_SWITCH)
    list(APPEND APP_SRC switch/crashlog.cpp ${PROJECT_SOURCE_DIR}/compat/switch/switch_compat.c)
elseif (PLATFORM_PS5)
    list(APPEND APP_SRC ps5/crashlog.cpp ps5/hbldr/hbldr.c ps5/hbldr/elfldr.c ps5/hbldr/pt.c
            screen/console.cpp screen/font.cpp screen/videoout_sdl.cpp)
elseif (PLATFORM_ANDROID)
    list(APPEND APP_SRC android/crashlog.cpp)
else ()
    list(APPEND APP_SRC linux/crashlog.cpp)
endif ()

# "0.1 (<commit>)" in the log and crash.log, as pPlay's APP_VERSION_FULL
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE P2P_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT P2P_COMMIT)
    set(P2P_COMMIT "unknown")
endif ()

add_executable(p2play ${APP_SRC})
target_link_libraries(p2play PRIVATE p2play_core)
target_compile_definitions(p2play PRIVATE ${P2P_PLATFORM_DEFINES}
        "APP_VERSION_FULL=\"${VERSION_MAJOR}.${VERSION_MINOR} (${P2P_COMMIT})\"")
if (PLATFORM_ANDROID)
    target_link_libraries(p2play PRIVATE log)
elseif (PLATFORM_LINUX)
    # the symbols of the backtrace in crash.log
    target_link_options(p2play PRIVATE -rdynamic)
endif ()
target_compile_options(p2play PRIVATE -g)

# Standalone outbound HTTPS transport. Android uses its system SSLSocket bridge; all
# other targets use libcurl, with OS trust on Linux and the platform CA source elsewhere.
if (NOT PLATFORM_ANDROID)
    find_package(CURL REQUIRED)
    target_include_directories(p2play PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(p2play PRIVATE ${CURL_LIBRARIES})
    if (PLATFORM_SWITCH)
        # Switch curl uses mbedTLS plus zlib/zstd (same link dependencies as pPlay).
        target_link_libraries(p2play PRIVATE mbedtls mbedx509 mbedcrypto zstd z)
    elseif (PLATFORM_PS4)
        # PS4 curl uses mbedTLS and zlib.
        target_link_libraries(p2play PRIVATE mbedtls mbedx509 mbedcrypto z)
    elseif (PLATFORM_PS5)
        # The PS5 portlib curl is static; keep all of its OpenSSL/PSL/compression
        # dependencies after libcurl, as in pPlay's PS5 link set.
        target_link_libraries(p2play PRIVATE psl ssl crypto zstd z m)
    endif ()
endif ()

if (PLATFORM_PS4 OR PLATFORM_PS5)
    file(READ ${PROJECT_SOURCE_DIR}/data/ca/cacert.pem P2P_CA_BUNDLE)
    configure_file(https_ca_bundle.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/https_ca_bundle.hpp @ONLY)
    target_include_directories(p2play PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(p2play PRIVATE P2P_HAS_EMBEDDED_CA=1)
endif ()

if (PLATFORM_LINUX)
    add_executable(p2play_https_test EXCLUDE_FROM_ALL
            ${PROJECT_SOURCE_DIR}/tools/https-transport-test.cpp https.cpp)
    target_include_directories(p2play_https_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/core ${CURL_INCLUDE_DIRS})
    target_compile_definitions(p2play_https_test PRIVATE P2P_HTTPS_TEST=1)
    target_link_libraries(p2play_https_test PRIVATE ${CURL_LIBRARIES})
endif ()

set(RELEASE_DIR ${CMAKE_BINARY_DIR}/release/p2play)
# the unstripped binary of the release, for crash.log offsets (CI: P2Play-Symbols-<platform>)
set(SYMBOLS_DIR ${CMAKE_BINARY_DIR}/symbols)
if (CMAKE_STRIP)
    set(P2P_STRIP ${CMAKE_STRIP})
else ()
    set(P2P_STRIP strip)
endif ()

if (PLATFORM_SWITCH)
    target_link_libraries(p2play PRIVATE nx)
    # as pPlay: the nro from an ELF without debug info, the full ELF kept
    add_custom_target(p2play_release
            DEPENDS p2play
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SYMBOLS_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:p2play> ${SYMBOLS_DIR}/p2play.elf
            COMMAND ${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-strip --strip-debug -o p2play.stripped.elf $<TARGET_FILE:p2play>
            COMMAND ${DEVKITPRO}/tools/bin/nacptool --create "P2Play" "freetushkan" "${VERSION_MAJOR}.${VERSION_MINOR}" p2play.nacp
            COMMAND ${DEVKITPRO}/tools/bin/elf2nro p2play.stripped.elf p2play.nro
                    --icon=${PROJECT_SOURCE_DIR}/data/switch/icon.jpg --nacp=p2play.nacp
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy p2play.nro ${RELEASE_DIR}/
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
elseif (PLATFORM_PS4)
    target_include_directories(p2play PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ps4)
    target_link_libraries(p2play PRIVATE SceSysmodule SceNet SceSystemService SceVideoOut kernel)
    # Linux constants in the musl headers, FreeBSD ones in libkernel (ps4/ps4_compat.c)
    target_link_options(p2play PRIVATE
            "LINKER:--wrap=clock_gettime"
            "LINKER:--wrap=clock_getres"
            "LINKER:--wrap=pthread_condattr_setclock"
            "LINKER:--wrap=send"
            "LINKER:--wrap=recv"
            "LINKER:--wrap=sendto"
            "LINKER:--wrap=recvfrom"
            "LINKER:--wrap=sendmsg"
            "LINKER:--wrap=recvmsg")
    set(PS4_PKG_DIR ${CMAKE_CURRENT_BINARY_DIR}/pkgroot)
    file(MAKE_DIRECTORY ${PS4_PKG_DIR})
    file(COPY ${PROJECT_SOURCE_DIR}/data/ps4/sce_sys ${PROJECT_SOURCE_DIR}/data/ps4/sce_module DESTINATION ${PS4_PKG_DIR})
    add_self(p2play)
    add_pkg(p2play ${PS4_PKG_DIR} "PTWO00001" "P2Play" "01.00" 0 "gde")
    add_custom_target(p2play_release
            DEPENDS p2play_pkg
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SYMBOLS_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:p2play> ${SYMBOLS_DIR}/p2play.elf
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/${PKG_OUT_NAME} ${RELEASE_DIR}/)
elseif (PLATFORM_PS5)
    # the big application (ps5/hbldr): a fake app of the launcher's title id; its log on the
    # screen through the SDK's SDL2 (a software surface)
    set(P2P_PS5_TITLE_ID "PTWO00005")
    target_compile_definitions(p2play PRIVATE P2P_PS5_TITLE_ID="${P2P_PS5_TITLE_ID}"
            HBLDR_TITLE_ID="${P2P_PS5_TITLE_ID}" HBLDR_TITLE_NAME="P2Play" HBLDR_PROCNAME="p2play")
    target_include_directories(p2play PRIVATE ${PS5_PAYLOAD_SDK}/target/user/homebrew/include)
    target_link_directories(p2play PRIVATE ${PS5_PAYLOAD_SDK}/target/user/homebrew/lib)
    target_link_libraries(p2play PRIVATE SDL2 SceNet SceVideoOut SceUserService SceSystemService SceAudioOut
            ScePad SceSysmodule SceImeDialog SceKeyboard iconv kernel_sys)
    set_target_properties(p2play PROPERTIES SUFFIX ".elf")
    # as pPlay: the payload without debug info, the full ELF kept
    add_custom_target(p2play_release
            DEPENDS p2play
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SYMBOLS_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:p2play> ${SYMBOLS_DIR}/p2play.elf
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${P2P_STRIP} --strip-debug -o ${RELEASE_DIR}/p2play.elf $<TARGET_FILE:p2play>)
else ()
    # Linux and Android: the service
    set_target_properties(p2play PROPERTIES OUTPUT_NAME p2playd)
    # as pPlay: a separate debug file and a debuglink to it
    if (CMAKE_OBJCOPY)
        set(P2P_OBJCOPY ${CMAKE_OBJCOPY})
    else ()
        set(P2P_OBJCOPY objcopy)
    endif ()
    add_custom_target(p2play_release
            DEPENDS p2play
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SYMBOLS_DIR}
            COMMAND ${P2P_OBJCOPY} --only-keep-debug $<TARGET_FILE:p2play> ${SYMBOLS_DIR}/p2playd.debug
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${P2P_STRIP} --strip-debug -o ${RELEASE_DIR}/p2playd $<TARGET_FILE:p2play>
            COMMAND ${P2P_OBJCOPY} --add-gnu-debuglink=${SYMBOLS_DIR}/p2playd.debug ${RELEASE_DIR}/p2playd
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/linux/p2playd.service ${RELEASE_DIR}/)
endif ()
