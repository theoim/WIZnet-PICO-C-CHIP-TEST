set(TARGET_NAME opcua_usb_stdio)

add_subdirectory(${CMAKE_SOURCE_DIR}/port/open62541
    ${CMAKE_BINARY_DIR}/port/open62541)

add_executable(${TARGET_NAME}
    src/main.c
    src/opcua_server.c
    src/opcua_tcp_probe.c
    src/wiznet_network.c
    src/usb_stdio_link.c
    src/data_table.c
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(${TARGET_NAME}
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_pio
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
