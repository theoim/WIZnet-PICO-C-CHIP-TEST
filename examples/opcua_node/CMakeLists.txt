set(TARGET_NAME opcua_node)

# 두 OPC UA 예제가 동시에 활성화되면 같은 바이너리 디렉터리를 두 번
# 쓰게 되어 CMake 가 중단된다. 먼저 처리된 쪽이 타깃을 만들므로 가드한다.
if(NOT TARGET OPEN62541_WIZNET)
    add_subdirectory(${CMAKE_SOURCE_DIR}/port/open62541
        ${CMAKE_BINARY_DIR}/port/open62541)
endif()

add_executable(${TARGET_NAME}
    src/main.c
    src/opcua_server.c
    src/opcua_tcp_probe.c
    src/wiznet_network.c
    src/usb_stdio_link.c
    src/data_table.c
    src/opcua_settings.c
    src/opcua_security.c
    src/sntp_client.c
)

# The certificates are generated, not committed: they hold private keys.
# tools/make_opcua_certs.sh writes src/opcua_certs.c and inc/opcua_certs.h, and
# both are git-ignored. Only the encryption build needs them.
if(OPCUA_ENABLE_ENCRYPTION)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/opcua_certs.c")
        message(FATAL_ERROR
            "opcua_certs.c is missing. Run tools/make_opcua_certs.sh first - "
            "the certificates are generated rather than committed because they "
            "contain private keys.")
    endif()
    target_sources(${TARGET_NAME} PRIVATE src/opcua_certs.c)

    # mbedtls_hardware_poll() has to be in this image. WIZ_ENTROPY_FILES is an
    # INTERFACE library, so linking it compiles wiz_entropy.c into this
    # executable rather than leaving the symbol in an archive the linker has
    # already walked past. It is linked here and nowhere else in this image.
    target_link_libraries(${TARGET_NAME} WIZ_ENTROPY_FILES)
endif()

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

# Stamp the image so the serial log says which build it is. Inferring that from
# behaviour has already cost one wrong conclusion.
execute_process(
        COMMAND git describe --always --dirty --abbrev=7
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE OPCUA_BUILD_GIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        )
if(NOT OPCUA_BUILD_GIT)
    set(OPCUA_BUILD_GIT "no-git")
endif()
string(TIMESTAMP OPCUA_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

target_compile_definitions(${TARGET_NAME} PRIVATE
        OPCUA_BUILD_GIT="${OPCUA_BUILD_GIT}"
        OPCUA_BUILD_UTC="${OPCUA_BUILD_UTC}"
        )

target_link_libraries(${TARGET_NAME}
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_flash
    hardware_sync
    hardware_watchdog
    ETHERNET_FILES
    IOLIBRARY_FILES
    PORT_COMMON_FILES
    TIMER_FILES
    OPEN62541_WIZNET
)

# memory_budget.md 의 채널 스윕(3/8/16/32)을 소스 수정 없이 재빌드하기 위한 값이다.
# 플래시에 저장된 설정이 없을 때 사용하는 기본 채널 수를 덮어쓴다.
set(OPCUA_DEFAULT_CHANNEL_COUNT 3 CACHE STRING
    "Default channel count when flash holds no settings blob (RAM sweep)")

target_compile_definitions(${TARGET_NAME} PRIVATE
    OPCUA_ENABLE=1
    OPCUA_TCP_PROBE_ENABLE=0
    OPCUA_DEFAULT_CHANNEL_COUNT=${OPCUA_DEFAULT_CHANNEL_COUNT}u
)

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
