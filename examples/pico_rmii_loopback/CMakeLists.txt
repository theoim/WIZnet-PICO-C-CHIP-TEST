set(TARGET_NAME pico_rmii_loopback)

add_executable(${TARGET_NAME}
    main.c
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${TARGET_NAME} PRIVATE
    pico_stdlib
    pico_multicore
    pico_rmii_ethernet
    ETHERNET_FILES
    IOLIBRARY_FILES
    PORT_COMMON_FILES
    LOOPBACK_FILES
)

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
