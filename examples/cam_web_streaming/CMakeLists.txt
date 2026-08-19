set(TARGET_NAME cam_web_streaming)

add_executable(${TARGET_NAME}
        main.c
        cam/cam_sensor.c
        cam/dvp_capture.c
        cam/sccb.c
        cam/xclk.c
        cam/sensor_res.c
        # Carried unmodified from esp32-camera 2.1.7. Everything it expects from
        # ESP-IDF is supplied by cam/esp_shim, so re-syncing with upstream is a
        # file copy rather than a merge.
        cam/ov3660.c
        )

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/cam
        # ov3660.c includes "esp_attr.h" and "freertos/task.h" by those exact
        # names; this is what makes the shims answer instead of ESP-IDF.
        ${CMAKE_CURRENT_LIST_DIR}/cam/esp_shim
        # w6300_rp2354b_cam_pins.h - the camera, microSD and microphone pins.
        ${CMAKE_SOURCE_DIR}/port/board
        )

pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/cam/dvp.pio)

target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib
        hardware_pio
        hardware_dma
        hardware_i2c
        hardware_pwm
        hardware_gpio
        hardware_clocks
        hardware_spi
        ETHERNET_FILES
        IOLIBRARY_FILES
        PORT_COMMON_FILES
        TIMER_FILES
        )

# USB CDC for the console: the board brings no UART header out, and GPIO0/1 are
# free header pins rather than a wired debug port.
pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
