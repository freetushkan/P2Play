set(SPIKE_SRC
        main.cpp
        http_server.cpp
        platform.cpp
        ram_cache.cpp
)

if (PLATFORM_PS4)
    list(APPEND SPIKE_SRC ps4/ps4_compat.c ps4/crashlog.c)
elseif (PLATFORM_SWITCH)
    list(APPEND SPIKE_SRC ${PROJECT_SOURCE_DIR}/compat/switch/switch_compat.c)
endif ()

add_executable(p2spike ${SPIKE_SRC})
target_link_libraries(p2spike PRIVATE torrent-rasterbar)
target_compile_definitions(p2spike PRIVATE ${P2P_PLATFORM_DEFINES})
target_compile_options(p2spike PRIVATE -g)

set(RELEASE_DIR ${CMAKE_BINARY_DIR}/release/p2spike)

if (PLATFORM_SWITCH)
    target_link_libraries(p2spike PRIVATE nx)
    add_custom_target(p2spike_release
            DEPENDS p2spike
            COMMAND ${DEVKITPRO}/tools/bin/nacptool --create "p2spike" "P2Play" "${VERSION_MAJOR}.${VERSION_MINOR}" p2spike.nacp
            COMMAND ${DEVKITPRO}/tools/bin/elf2nro $<TARGET_FILE:p2spike> p2spike.nro
                    --icon=${PROJECT_SOURCE_DIR}/data/switch/icon.jpg --nacp=p2spike.nacp
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy p2spike.nro ${RELEASE_DIR}/
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
elseif (PLATFORM_PS4)
    target_include_directories(p2spike PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ps4)
    target_link_libraries(p2spike PRIVATE SceSysmodule SceNet SceSystemService kernel)
    # Linux constants in the musl headers, FreeBSD ones in libkernel (ps4/ps4_compat.c)
    target_link_options(p2spike PRIVATE
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
    add_self(p2spike)
    add_pkg(p2spike ${PS4_PKG_DIR} "PTWO00001" "p2spike" "01.00" 0 "gde")
    add_custom_target(p2spike_release
            DEPENDS p2spike_pkg
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/${PKG_OUT_NAME} ${RELEASE_DIR}/)
elseif (PLATFORM_PS5)
    target_link_libraries(p2spike PRIVATE SceNet kernel_sys)
    set_target_properties(p2spike PROPERTIES SUFFIX ".elf")
    add_custom_target(p2spike_release
            DEPENDS p2spike
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:p2spike> ${RELEASE_DIR}/)
else ()
    add_custom_target(p2spike_release
            DEPENDS p2spike
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RELEASE_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:p2spike> ${RELEASE_DIR}/)
endif ()
