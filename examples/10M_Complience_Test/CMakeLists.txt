set(TARGET_NAME 10M_Complience_Test)

add_executable(${TARGET_NAME}
        ${TARGET_NAME}.c
        )

target_link_libraries(${TARGET_NAME}
        pico_stdlib
        pico_rand
        hardware_spi
        hardware_uart
        hardware_dma
        hardware_pio
        ETHERNET_FILES
        IOLIBRARY_FILES
        PORT_COMMON_FILES
        LOOPBACK_FILES
        TIMER_FILES
        W6300_FILES        
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
