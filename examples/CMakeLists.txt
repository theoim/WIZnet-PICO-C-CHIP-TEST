# Bring-up target for a new board: the smallest thing that proves the chip
# answers. Enabled here rather than in the commented block above because on
# W6300_RP2354B_CAM it is the first test that has to pass.
add_subdirectory(loopback)

# add_subdirectory(buffer_read)
# add_subdirectory(PING_TEST)
# add_subdirectory(10M_Complience_Test)
# add_subdirectory(opcua_usb_stdio)
# add_subdirectory(opcua_node)   # Phase A product prototype (see examples/opcua_node/TASK_BRIEF.md)
# add_subdirectory(wiz_claw_agent)
# Needs PICO_DEFAULT_LED_PIN, which the RP2354-B camera board does not define --
# no LED has been identified on its schematic yet, and inventing one would build
# cleanly and blink nothing. Left out for that board rather than guessing.
if(NOT ${BOARD_NAME} STREQUAL W6300_RP2354B_CAM)
    add_subdirectory(wiz_claw_spi_host)
endif()

# MII ethernet examples -- require W55RP20_EVB_PICO
if(${BOARD_NAME} STREQUAL W55RP20_EVB_PICO)
    add_subdirectory(pico_rmii_loopback)
    add_subdirectory(pico_rmii_gateway)
endif()