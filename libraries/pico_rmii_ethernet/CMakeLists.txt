add_library(pico_rmii_ethernet INTERFACE)

target_sources(pico_rmii_ethernet INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/rmii_ethernet.c
)

target_include_directories(pico_rmii_ethernet INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_generate_pio_header(pico_rmii_ethernet
    ${CMAKE_CURRENT_LIST_DIR}/rmii_ethernet_phy_rx.pio
)
pico_generate_pio_header(pico_rmii_ethernet
    ${CMAKE_CURRENT_LIST_DIR}/rmii_ethernet_phy_tx.pio
)

target_link_libraries(pico_rmii_ethernet INTERFACE
    hardware_pio
    hardware_dma
    pico_stdlib
    pico_unique_id
    pico_lwip_nosys
)
