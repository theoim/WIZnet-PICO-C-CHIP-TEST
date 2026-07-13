# add_subdirectory(buffer_read)
# add_subdirectory(PING_TEST)
# add_subdirectory(10M_Complience_Test)
# add_subdirectory(opcua_usb_stdio)
add_subdirectory(opcua_node)   # Phase A product prototype (see examples/opcua_node/TASK_BRIEF.md)
# add_subdirectory(wiz_claw_agent)
add_subdirectory(wiz_claw_spi_host)

# MII ethernet examples -- require W55RP20_EVB_PICO
if(${BOARD_NAME} STREQUAL W55RP20_EVB_PICO)
    add_subdirectory(pico_rmii_loopback)
    add_subdirectory(pico_rmii_gateway)
endif()