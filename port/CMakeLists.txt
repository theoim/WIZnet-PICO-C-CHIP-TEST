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

# ── WIZ_ENTROPY_FILES: mbedTLS 엔트로피 소스 ─────────────────────
#
# wiz_claw_tls.c 안에 file-static 으로 들어 있던 콜백을 공용으로 뽑은 것이다.
# OPC UA 서버가 cJSON 과 wiz_claw 전체를 끌어오지 않고 쓰려면 분리가 필요했다.
# 하드웨어 RNG 가 없는 부품에서는 wiz_entropy.h 가 빌드를 멈춘다 —
# 쓰는 쪽이 WIZ_ENTROPY_ALLOW_NO_TRNG 로 의식적으로 허용해야 한다.

# INTERFACE 라이브러리인 이유.
#
# mbedcrypto 가 mbedtls_hardware_poll() 을 필요로 하는데 이 코드는 mbedcrypto 의
# 헤더가 필요하다. 의존이 양방향이라 정적 라이브러리로는 링커가 아카이브를 훑는
# 순서에 걸리고, OBJECT 로 만들면 pico_stdlib 같은 SDK INTERFACE 라이브러리가
# 자기 소스를 이 타깃에도 컴파일해 넣어 실행 파일과 중복 정의가 난다.
#
# INTERFACE 는 아무것도 컴파일하지 않고 소비자에게 소스를 넘긴다. 결과적으로
# wiz_entropy.c 가 소비자 안에서 컴파일되므로 심볼은 항상 최종 링크에 있다.
# 단, 한 실행 파일에 두 경로로 들어가면 또 중복이므로 소비자는 하나만 링크한다.
add_library(WIZ_ENTROPY_FILES INTERFACE)

target_sources(WIZ_ENTROPY_FILES INTERFACE
    ${PORT_DIR}/mbedtls/src/wiz_entropy.c
)

target_include_directories(WIZ_ENTROPY_FILES INTERFACE
    ${PORT_DIR}/mbedtls/inc
)

target_link_libraries(WIZ_ENTROPY_FILES INTERFACE
    pico_stdlib
    pico_rand
    mbedcrypto
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
    # MBEDTLS_ENTROPY_HARDWARE_ALT 이후로 mbedcrypto 를 쓰는 모든 타깃이
    # mbedtls_hardware_poll() 을 제공해야 링크된다.
    WIZ_ENTROPY_FILES
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
