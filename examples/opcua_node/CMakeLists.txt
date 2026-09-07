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
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(${TARGET_NAME}
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_flash
    hardware_sync
    ETHERNET_FILES
    IOLIBRARY_FILES
    PORT_COMMON_FILES
    TIMER_FILES
    OPEN62541_WIZNET
)

target_compile_definitions(${TARGET_NAME} PRIVATE
    OPCUA_ENABLE=1
    OPCUA_TCP_PROBE_ENABLE=0
)

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
