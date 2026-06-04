# ioLibrary_Driver
add_library(IOLIBRARY_FILES STATIC)

target_sources(IOLIBRARY_FILES PUBLIC
        ${PORT_DIR}/ioLibrary_Driver/src/wizchip_spi.c
        ${PORT_DIR}/ioLibrary_Driver/src/wizchip_gpio_irq.c
        )

if(${BOARD_NAME} STREQUAL W55RP20_EVB_PICO OR ${WIZNET_CHIP} STREQUAL W6300)
pico_generate_pio_header(IOLIBRARY_FILES ${PORT_DIR}/ioLibrary_Driver/src/wizchip_qspi_pio.pio)

target_sources(IOLIBRARY_FILES PUBLIC 
        ${PORT_DIR}/ioLibrary_Driver/src/wizchip_qspi_pio.c
)

target_include_directories(IOLIBRARY_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${PORT_DIR}/ioLibrary_Driver/inc
        ${PORT_DIR}
)
endif()

target_include_directories(IOLIBRARY_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${PORT_DIR}/ioLibrary_Driver/inc
        ${PORT_DIR}
        )

target_link_libraries(IOLIBRARY_FILES PUBLIC
        pico_stdlib
        hardware_pio
        hardware_spi
        hardware_dma
        hardware_clocks
        )

# timer
add_library(TIMER_FILES STATIC)

target_sources(TIMER_FILES PUBLIC
        ${PORT_DIR}/timer/timer.c
        )

target_include_directories(TIMER_FILES PUBLIC
        ${PORT_DIR}/timer
        )

target_link_libraries(TIMER_FILES PRIVATE
        pico_stdlib      
        )


# board
add_library(BOARD_FILES STATIC)

target_sources(BOARD_FILES PUBLIC
        ${PORT_DIR}/board/can/can.c
        )

pico_generate_pio_header(BOARD_FILES ${PORT_DIR}/board/can/can.pio)

target_include_directories(BOARD_FILES PUBLIC
        ${PORT_DIR}/board/can
        )

target_link_libraries(BOARD_FILES PRIVATE
        pico_stdlib      
        cmsis_core
        hardware_pio
        hardware_dma
        )

# port_common
add_library(PORT_COMMON_FILES STATIC)

target_include_directories(PORT_COMMON_FILES PUBLIC
${PORT_DIR}
)

target_link_libraries(PORT_COMMON_FILES PUBLIC
pico_stdlib
hardware_clocks
hardware_spi
hardware_dma
hardware_pio
)

# ── cJSON (FetchContent — CMake 구성 시 자동 다운로드) ───────────
include(FetchContent)

FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cjson)

# ── wiz-claw: Agent 코어 + WIZnet 이더넷/HTTP/TLS 포팅 계층 ──────
#
#   core — 플랫폼 독립 (cJSON만 의존)
#   net  — ioLibrary DNS/DHCP 래퍼
#   http — WIZnet TCP 소켓 위 HTTP/1.1 클라이언트
#   tls  — mbedTLS + RP2350 TRNG 엔트로피

add_library(wiz_claw STATIC
    ${PORT_DIR}/wiz-claw/src/wiz_claw_telegram.c
    ${PORT_DIR}/wiz-claw/src/wiz_claw_llm.c
    ${PORT_DIR}/wiz-claw/src/wiz_claw_agent.c
    ${PORT_DIR}/wiz-claw/src/wiz_claw_net.c
    ${PORT_DIR}/wiz-claw/src/wiz_claw_http.c
    ${PORT_DIR}/wiz-claw/src/wiz_claw_tls.c
)

target_include_directories(wiz_claw PUBLIC
    ${PORT_DIR}/wiz-claw/include
)

target_link_libraries(wiz_claw PRIVATE
        DNS_FILES
        DHCP_FILES
)

# cJSON
if(TARGET cjson)
    target_link_libraries(wiz_claw PUBLIC cjson)
elseif(DEFINED CJSON_INCLUDE_DIR)
    target_include_directories(wiz_claw PUBLIC ${CJSON_INCLUDE_DIR})
else()
    message(WARNING "wiz_claw: cJSON 경로를 CJSON_INCLUDE_DIR 로 지정하세요.")
endif()

# ioLibrary + Pico SDK + RP2350 TRNG
target_link_libraries(wiz_claw PUBLIC
    cjson
    IOLIBRARY_FILES
    pico_stdlib
    pico_rand         # RP2350 하드웨어 TRNG (get_rand_32)
    mbedtls
    mbedcrypto
    mbedx509
)

# mbedTLS
if(TARGET mbedtls)
    target_link_libraries(wiz_claw PUBLIC mbedtls)
elseif(DEFINED MBEDTLS_DIR)
    target_include_directories(wiz_claw PUBLIC ${MBEDTLS_DIR}/include)
    target_link_libraries(wiz_claw PUBLIC mbedtls mbedcrypto mbedx509)
else()
    message(WARNING "wiz_claw: mbedTLS를 찾을 수 없습니다. "
                    "MBEDTLS_DIR 또는 mbedtls CMake 타겟을 설정하세요.")
endif()
