set(TARGET_NAME wiz_claw_agent)

add_executable(${TARGET_NAME}
        main.c
        )

target_link_libraries(${TARGET_NAME} PRIVATE
        hardware_gpio
        pico_stdlib
        hardware_spi
        hardware_dma
        hardware_pio
        ETHERNET_FILES
        IOLIBRARY_FILES
        PORT_COMMON_FILES
        wiz_claw
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
