set(TARGET_NAME wiz_claw_spi_host)

add_executable(${TARGET_NAME}
        main.c
        wiz_spi_slave.c
        wiz_claw_webserver.c
        )

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        )

target_link_libraries(${TARGET_NAME} PRIVATE
        hardware_gpio
        hardware_spi
        hardware_dma
        hardware_pio
        hardware_flash
        hardware_sync
        hardware_watchdog
        pico_stdlib
        pico_multicore
        ETHERNET_FILES
        IOLIBRARY_FILES
        PORT_COMMON_FILES
        wiz_claw
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
