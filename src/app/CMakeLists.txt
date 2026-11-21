# P2Play, the application around the core: p2playd on Linux and Android, a homebrew
# application on Switch, PS4 and PS5
set(APP_SRC
        main.cpp
        platform.cpp
)

if (PLATFORM_PS4)
    list(APPEND APP_SRC ps4/ps4_compat.c ps4/crashlog.c)
elseif (PLATFORM_SWITCH)
    list(APPEND APP_SRC ${PROJECT_SOURCE_DIR}/compat/switch/switch_compat.c)
endif ()

add_executable(p2play ${APP_SRC})
target_link_libraries(p2play PRIVATE p2play_core)
target_compile_definitions(p2play PRIVATE ${P2P_PLATFORM_DEFINES})
target_compile_options(p2play PRIVATE -g)

set(RELEASE_DIR ${CMAKE_BINARY_DIR}/release/p2play)

if (PLATFORM_SWITCH)
    target_link_libraries(p2play PRIVATE nx)
    add_custom_target(p2play_release
            DEPENDS p2play
            COMMAND ${DEVKITPRO}/tools/bin/nacptool --create "P2Play" "freetushkan" "${VERSION_MAJOR}.${VERSION_MINOR}" p2play.nacp
            COMMAND ${DEVKITPRO}/tools/bin/elf2nro $<TARGET_FILE:p2play> p2play.nro
                    --icon=${PROJECT_SOURCE_DIR}/data/switch/icon.jpg --nacp=p2play.nacp
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy p2play.nro ${RELEASE_DIR}/
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
elseif (PLATFORM_PS4)
    target_include_directories(p2play PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ps4)
    target_link_libraries(p2play PRIVATE SceSysmodule SceNet SceSystemService kernel)
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
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/${PKG_OUT_NAME} ${RELEASE_DIR}/)
elseif (PLATFORM_PS5)
    target_link_libraries(p2play PRIVATE SceNet kernel_sys)
    set_target_properties(p2play PROPERTIES SUFFIX ".elf")
    add_custom_target(p2play_release
            DEPENDS p2play
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:p2play> ${RELEASE_DIR}/)
else ()
    # Linux and Android: the service
    set_target_properties(p2play PROPERTIES OUTPUT_NAME p2playd)
    add_custom_target(p2play_release
            DEPENDS p2play
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:p2play> ${RELEASE_DIR}/
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/linux/p2playd.service ${RELEASE_DIR}/)
endif ()
