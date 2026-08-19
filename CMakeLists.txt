# CMake minimum required version
cmake_minimum_required(VERSION 3.12)
 
# Set board
# set(BOARD_NAME WIZnet_Ethernet_HAT)
# set(BOARD_NAME W5100S_EVB_PICO)
# set(BOARD_NAME W5500_EVB_PICO)
# set(BOARD_NAME W55RP20_EVB_PICO)
# set(BOARD_NAME W6100_EVB_PICO)
# set(BOARD_NAME W6300_EVB_PICO)
# set(BOARD_NAME W5500_EVB_PICO2)
# set(BOARD_NAME W6100_EVB_PICO2)
# set(BOARD_NAME W6300_EVB_PICO2)
set(BOARD_NAME W6300_RP2354B_CAM)


if(${BOARD_NAME} STREQUAL WIZnet_Ethernet_HAT)
    #set(PICO_PLATFORM rp2350) #If you are using WIZnet_ethernet_HAT with Pico2, uncomment it.
    set(WIZNET_CHIP W5100S)
    add_definitions(-D_WIZCHIP_=W5100S)
    add_definitions(-DDEVICE_BOARD_NAME=WIZnet_Ethernet_HAT)
    
elseif(${BOARD_NAME} STREQUAL W5100S_EVB_PICO)
    set(WIZNET_CHIP W5100S)
    add_definitions(-D_WIZCHIP_=W5100S)
    add_definitions(-DDEVICE_BOARD_NAME=W5100S_EVB_PICO)
    
elseif(${BOARD_NAME} STREQUAL W5500_EVB_PICO)
    set(WIZNET_CHIP W5500)
    add_definitions(-D_WIZCHIP_=W5500)
    add_definitions(-DDEVICE_BOARD_NAME=W5500_EVB_PICO)

elseif(${BOARD_NAME} STREQUAL W55RP20_EVB_PICO)
    set(WIZNET_CHIP W5500)
    add_definitions(-D_WIZCHIP_=W5500)
    add_definitions(-DDEVICE_BOARD_NAME=W55RP20_EVB_PICO)

elseif(${BOARD_NAME} STREQUAL W6100_EVB_PICO)
    set(WIZNET_CHIP W6100)
    add_definitions(-D_WIZCHIP_=W6100)
    add_definitions(-DDEVICE_BOARD_NAME=W6100_EVB_PICO)

elseif(${BOARD_NAME} STREQUAL W6300_EVB_PICO)
    set(WIZNET_CHIP W6300)
    add_definitions(-D_WIZCHIP_=W6300)
    add_definitions(-DDEVICE_BOARD_NAME=W6300_EVB_PICO)
# Set QSPI MODE for W6300
    set(WIZCHIP_QSPI_MODE QSPI_QUAD_MODE)
    # set(WIZCHIP_QSPI_MODE QSPI_DUAL_MODE)
    # set(WIZCHIP_QSPI_MODE QSPI_SINGLE_MODE)
    
    add_definitions(-D_WIZCHIP_QSPI_MODE_=${WIZCHIP_QSPI_MODE})

elseif(${BOARD_NAME} STREQUAL W5100S_EVB_PICO2)
    set(PICO_PLATFORM rp2350)
    set(WIZNET_CHIP W5100S)
    add_definitions(-D_WIZCHIP_=W5100S)
    add_definitions(-DDEVICE_BOARD_NAME=W5100S_EVB_PICO2)
    
elseif(${BOARD_NAME} STREQUAL W5500_EVB_PICO2)
    set(PICO_PLATFORM rp2350)
    set(WIZNET_CHIP W5500)
    add_definitions(-D_WIZCHIP_=W5500)
    add_definitions(-DDEVICE_BOARD_NAME=W5500_EVB_PICO2)

elseif(${BOARD_NAME} STREQUAL W6100_EVB_PICO2)
    set(PICO_PLATFORM rp2350)
    set(WIZNET_CHIP W6100)
    add_definitions(-D_WIZCHIP_=W6100)
    add_definitions(-DDEVICE_BOARD_NAME=W6100_EVB_PICO2)

elseif(${BOARD_NAME} STREQUAL W6300_EVB_PICO2)
    set(PICO_PLATFORM rp2350)
    set(WIZNET_CHIP W6300)
    add_definitions(-D_WIZCHIP_=W6300)
    add_definitions(-DDEVICE_BOARD_NAME=W6300_EVB_PICO2)
# Set QSPI MODE for W6300
    # Set QSPI MODE for W6300
    set(WIZCHIP_QSPI_MODE QSPI_QUAD_MODE)
    # set(WIZCHIP_QSPI_MODE QSPI_DUAL_MODE)
    # set(WIZCHIP_QSPI_MODE QSPI_SINGLE_MODE)
    
    add_definitions(-D_WIZCHIP_QSPI_MODE_=${WIZCHIP_QSPI_MODE})

elseif(${BOARD_NAME} STREQUAL W6300_RP2354B_CAM)
    # RP2354-B + W6300, with a DVP camera, a microSD socket and a PDM mic.
    #
    # PICO_BOARD is what makes this build different from every other entry
    # above. The others run on stock Raspberry Pi boards, so the SDK's own board
    # header describes them. This one does not exist upstream, so the project
    # supplies its own -- see boards/w6300_rp2354b_cam.h, and the
    # PICO_BOARD_HEADER_DIRS line further down that lets the SDK find it.
    #
    # Getting this wrong is not a build error. PICO_BOARD defaults to pico2,
    # which sets PICO_RP2350A=1, which tells the SDK this part has 30 GPIOs.
    # Everything above GPIO29 -- the whole camera bus, both SCCB lines, the
    # microphone -- then quietly stops working while the build stays green.
    set(PICO_PLATFORM rp2350)
    set(PICO_BOARD w6300_rp2354b_cam)

    set(WIZNET_CHIP W6300)
    add_definitions(-D_WIZCHIP_=W6300)
    add_definitions(-DDEVICE_BOARD_NAME=W6300_RP2354B_CAM)

    # QSPI mode for the W6300. All four data lines are wired on this board
    # (GPIO16..19), so quad is available; drop to single while bringing the
    # board up, because a single-line failure is much easier to read on a scope.
    set(WIZCHIP_QSPI_MODE QSPI_QUAD_MODE)
    # set(WIZCHIP_QSPI_MODE QSPI_DUAL_MODE)
    # set(WIZCHIP_QSPI_MODE QSPI_SINGLE_MODE)

    add_definitions(-D_WIZCHIP_QSPI_MODE_=${WIZCHIP_QSPI_MODE})

else()
    message(FATAL_ERROR "BOARD_NAME is wrong = ${BOARD_NAME}")
endif()

# Board headers this project supplies itself.
#
# The SDK appends its own boards directory to this list inside
# cmake/generic_board.cmake and then searches the whole list for
# ${PICO_BOARD}.h, so an out-of-tree header only has to be on the list before
# pico_sdk_init() runs. Putting it here rather than in
# libraries/pico-sdk/src/boards keeps it from being erased the next time the
# submodule is updated.
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

message(STATUS "WIZNET_CHIP = ${WIZNET_CHIP}")


if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH ${CMAKE_SOURCE_DIR}/libraries/pico-sdk)
    message(STATUS "PICO_SDK_PATH = ${PICO_SDK_PATH}")
endif()

include(pico_sdk_import.cmake)
include(wiznet_pico_c_sdk_version.cmake)

# Set project name
set(PROJECT_NAME WIZNET-PICO-C)

# Set project informations
project(${PROJECT_NAME} C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

# Set the project root directory if it's not already defined, as may happen if
# the tests folder is included directly by a parent project, without including
# the top level CMakeLists.txt.
if(NOT DEFINED WIZNET_DIR)
    set(WIZNET_DIR ${CMAKE_SOURCE_DIR}/libraries/ioLibrary_Driver)
    message(STATUS "WIZNET_DIR = ${WIZNET_DIR}")
endif()

if(NOT DEFINED MBEDTLS_DIR)
    set(MBEDTLS_DIR ${CMAKE_SOURCE_DIR}/libraries/mbedtls)
    message(STATUS "MBEDTLS_DIR = ${MBEDTLS_DIR}")
endif()

if(NOT DEFINED PORT_DIR)
    set(PORT_DIR ${CMAKE_SOURCE_DIR}/port)
    message(STATUS "PORT_DIR = ${PORT_DIR}")
endif()

# Turn off mbedtls test mode 
set(ENABLE_PROGRAMS OFF CACHE BOOL "Build mbedtls programs")
set(ENABLE_TESTING OFF CACHE BOOL "Build mbedtls testing")
add_definitions(-DMBEDTLS_CONFIG_FILE="${PORT_DIR}/mbedtls/inc/ssl_config.h")
add_definitions(-DSET_TRUSTED_CERT_IN_SAMPLES)
add_definitions(-DPICO_USE_FASTEST_SUPPORTED_CLOCK=1)

# Set compile options
add_compile_options(
        -Wall
        -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
        -Wno-unused-function # we have some for the docs that aren't called
        -Wno-maybe-uninitialized
        )

# Hardware-specific examples in subdirectories:
add_subdirectory(examples)

# Add libraries in subdirectories
add_subdirectory(${CMAKE_SOURCE_DIR}/libraries)
add_subdirectory(${MBEDTLS_DIR})
add_subdirectory(${PORT_DIR})

