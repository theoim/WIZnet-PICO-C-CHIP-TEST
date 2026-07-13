/**
 * wiz_spi_slave.c — RP2350 SPI0 슬레이브 드라이버
 *
 * 역할:
 *   - SPI0 슬레이브 모드 초기화 (GP0~GP3, IRQ=GP4)
 *   - W6300 QSPI(GP15-GP22) 간섭 회피를 위해 SPI1(GP10-GP14)에서 이동
 *   - IRQ 핀(GP4)으로 ESP32에 데이터준비 신호 전달
 *   - wiz_claw_spi_proto.h 프로토콜 파싱 (magic/crc 검증)
 *   - CMD_PING 수신 시 CMD_PONG 즉시 응답
 *   - 그 외 CMD는 등록된 rx_cb로 위임
 */

#include "wiz_spi_slave.h"

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

/* ── 모듈 상태 ──────────────────────────────────────────────────── */
static wiz_spi_slave_rx_cb_t     s_rx_cb;
static void                     *s_rx_ctx;
static wiz_spi_slave_crc_err_cb_t s_crc_err_cb;
static void                     *s_crc_err_ctx;
static uint8_t                   s_seq;   /* 송신 시퀀스 카운터 */

void wiz_spi_slave_set_crc_err_cb(wiz_spi_slave_crc_err_cb_t cb, void *user_ctx)
{
    s_crc_err_cb  = cb;
    s_crc_err_ctx = user_ctx;
}

/* ── CRC 계산 ────────────────────────────────────────────────────
 * 헤더의 crc 필드를 0으로 두고 hdr+payload 전체 XOR.
 */
static uint8_t _calc_crc(const uint8_t *hdr_bytes, const uint8_t *payload, uint16_t plen)
{
    uint8_t crc = 0;

    /* 헤더 7바이트 (crc 필드는 이미 0으로 세팅되어 있어야 함) */
    for (size_t i = 0; i < SPI_CLAW_HDR_SIZE; i++) {
        crc ^= hdr_bytes[i];
    }
    for (uint16_t i = 0; i < plen; i++) {
        crc ^= payload[i];
    }
    return crc;
}

/* ── RX FIFO 직접 읽기 (TX FIFO 오염 없음) ──────────────────────────
 * spi_read_blocking은 RX마다 TX에 0x00을 써서 TX FIFO를 오염시킨다.
 * CS 해제 후 TX FIFO에 남은 0x00이 다음 PONG 앞에 나가 ESP32가
 * 잘못된 데이터를 받는 버그를 막기 위해 DR 레지스터를 직접 읽는다.
 */
static inline void _rx_bytes(uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        while (!spi_is_readable(spi0)) { tight_loop_contents(); }
        dst[i] = (uint8_t)spi_get_hw(spi0)->dr;
    }
}

/* TX FIFO 잔여 바이트 폐기 (PONG 전송 전 클린업) */
static inline void _flush_tx_fifo(void)
{
    spi_inst_t *spi = spi0;
    hw_clear_bits(&spi_get_hw(spi)->cr1, SPI_SSPCR1_SSE_BITS);
    while (spi_is_readable(spi)) { (void)spi_get_hw(spi)->dr; }
    hw_set_bits(&spi_get_hw(spi)->cr1, SPI_SSPCR1_SSE_BITS);
}

/* ── CMD_PONG 전송 ───────────────────────────────────────────────
 * 흐름:
 *   1. TX FIFO 클린업
 *   2. IRQ 핀 assert → ESP32가 SPI 전송 시작
 *   3. spi_write_blocking → TX FIFO에 로드 후 마스터가 클럭하면 전송 완료
 *   4. IRQ 핀 deassert
 */
static void _send_pong(uint8_t req_seq)
{
    spi_claw_hdr_t pong;
    memset(&pong, 0, sizeof(pong));
    pong.magic[0] = SPI_CLAW_MAGIC_0;
    pong.magic[1] = SPI_CLAW_MAGIC_1;
    pong.cmd      = SPI_CMD_PONG;
    pong.len      = 0;
    pong.seq      = req_seq;
    pong.crc      = 0;
    pong.crc      = _calc_crc((const uint8_t *)&pong, NULL, 0);

    _flush_tx_fifo();
    gpio_put(WIZ_SPI_SLAVE_IRQ_PIN, 1);
    spi_write_blocking(spi0, (const uint8_t *)&pong, SPI_CLAW_HDR_SIZE);
    gpio_put(WIZ_SPI_SLAVE_IRQ_PIN, 0);

    printf("[spi_slave] CMD_PONG sent (seq=%u)\n", req_seq);
}

/* ── 패킷 수신 처리 ──────────────────────────────────────────────────
 * magic[0]+magic[1]은 wiz_spi_slave_poll의 슬라이딩 윈도우 스캔이
 * 이미 소비했으므로 여기서는 나머지 5바이트(cmd~crc)만 읽는다.
 */
static void _handle_rx(void)
{
    uint8_t hdr_buf[SPI_CLAW_HDR_SIZE];
    hdr_buf[0] = SPI_CLAW_MAGIC_0;
    hdr_buf[1] = SPI_CLAW_MAGIC_1;

    /* 나머지 헤더 5바이트: cmd, len_lo, len_hi, seq, crc */
    _rx_bytes(&hdr_buf[2], SPI_CLAW_HDR_SIZE - 2);

    uint16_t plen = (uint16_t)(hdr_buf[3] | ((uint16_t)hdr_buf[4] << 8));
    /* Static: SPI_CLAW_MAX_CHUNK (4096) on stack would overflow core1's default 4KB stack. */
    static uint8_t payload[SPI_CLAW_MAX_CHUNK];

    if (plen > SPI_CLAW_MAX_CHUNK) {
        printf("[spi_slave] payload too large: %u\n", plen);
        return;
    }
    if (plen > 0) {
        _rx_bytes(payload, plen);
    }

    /* CRC 검증 */
    uint8_t received_crc = hdr_buf[6];
    hdr_buf[6] = 0;
    uint8_t calc_crc = _calc_crc(hdr_buf, plen > 0 ? payload : NULL, plen);

    if (calc_crc != received_crc) {
        printf("[spi_slave] CRC error: got 0x%02X, expected 0x%02X\n",
               received_crc, calc_crc);
        printf("[spi_slave]   hdr: %02X %02X %02X %02X %02X %02X %02X\n",
               hdr_buf[0], hdr_buf[1], hdr_buf[2], hdr_buf[3],
               hdr_buf[4], hdr_buf[5], received_crc);
        if (plen > 0 && plen <= 32) {
            printf("[spi_slave]   payload[0..%u]:", plen);
            for (uint16_t _i = 0; _i < plen; _i++) printf(" %02X", payload[_i]);
            printf("\n");
        }
        if (s_crc_err_cb) {
            s_crc_err_cb(hdr_buf[2], s_crc_err_ctx);
        }
        return;
    }

    uint8_t cmd = hdr_buf[2];
    uint8_t seq = hdr_buf[5];
    /* CHUNK_DATA / HTTP_BODY 수신 직후 printf ~3ms → SSP FIFO(8 bytes) overflow → 다음 chunk 손실 방지 */
    if (cmd != SPI_CMD_CHUNK_DATA && cmd != SPI_CMD_HTTP_BODY) {
        printf("[spi_slave] RX cmd=0x%02X seq=%u len=%u\n", cmd, seq, plen);
    }

    switch ((spi_claw_cmd_t)cmd) {
    case SPI_CMD_PING:
        _send_pong(seq);
        if (s_rx_cb) {
            s_rx_cb(SPI_CMD_PING, seq, NULL, 0, s_rx_ctx);
        }
        break;

    default:
        if (s_rx_cb) {
            s_rx_cb((spi_claw_cmd_t)cmd, seq, plen > 0 ? payload : NULL, plen, s_rx_ctx);
        }
        break;
    }
}

/* ── 공개 API ────────────────────────────────────────────────── */

void wiz_spi_slave_init(wiz_spi_slave_rx_cb_t rx_cb, void *user_ctx)
{
    s_rx_cb  = rx_cb;
    s_rx_ctx = user_ctx;
    s_seq    = 0;

    /* ── SPI0 슬레이브 모드 초기화 ──────────────────────────────────
     * RP2040/RP2350 TRM: MS 비트는 SSE=0일 때만 기록 가능.
     * pico-sdk spi_set_slave()는 SSE=1 상태에서 MS를 쓰기 때문에
     * 하드웨어가 쓰기를 무시하고 master mode에 머물 수 있다.
     * 올바른 순서: spi_init → SSE=0 → MS=1 → format → SSE=1
     */
    /* Slave 모드: 최대 수신 SCK = SSPCLK/(SSPCPSR×(SCR+1)).
     * SSPCPSR=2, SCR=0 → 62.5MHz 기준 → 8MHz ESP32 SCK 7.8× 오버샘플링 보장. */
    spi_init(spi0, 62500000);

    /* SSE=0 로 비활성화 후 MS 비트 설정 (TRM 요구사항) */
    hw_clear_bits(&spi_get_hw(spi0)->cr1, SPI_SSPCR1_SSE_BITS);
    spi_set_slave(spi0, true);
    /* 명시적 format: 8-bit, mode 0 (CPOL=0 CPHA=0), MSB-first */
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST); /* mode 1: CPHA=0 errata 회피 */
    /* SSE=1 다시 활성화 */
    hw_set_bits(&spi_get_hw(spi0)->cr1, SPI_SSPCR1_SSE_BITS);

    gpio_set_function(WIZ_SPI_SLAVE_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(WIZ_SPI_SLAVE_TX_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(WIZ_SPI_SLAVE_RX_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(WIZ_SPI_SLAVE_CSN_PIN, GPIO_FUNC_SPI);
    /* CSn 풀업: 미연결 시 floating-LOW 방지 (SSP 항상-선택 버그 차단).
     * SCK 풀다운: mode 0 idle LOW 강제 (노이즈 클럭 차단).
     * MOSI 풀다운: 미연결 시 0x00 수신 → 0xCA 잡음 구분 가능. */
    gpio_set_pulls(WIZ_SPI_SLAVE_CSN_PIN, true,  false);  /* pull-up  */
    gpio_set_pulls(WIZ_SPI_SLAVE_SCK_PIN, false, true);   /* pull-down */
    gpio_set_pulls(WIZ_SPI_SLAVE_RX_PIN,  false, true);   /* pull-down */

    /* IRQ 출력 핀 */
    gpio_init(WIZ_SPI_SLAVE_IRQ_PIN);
    gpio_set_dir(WIZ_SPI_SLAVE_IRQ_PIN, GPIO_OUT);
    gpio_put(WIZ_SPI_SLAVE_IRQ_PIN, 0);

    /* GPIO 함수 할당 전후 잡음으로 쌓인 RX FIFO 스테일 바이트 제거 */
    while (spi_is_readable(spi0)) { (void)spi_get_hw(spi0)->dr; }

    printf("[spi_slave] init done (SCK=%d TX=%d RX=%d CSn=%d IRQ=%d)\n",
           WIZ_SPI_SLAVE_SCK_PIN, WIZ_SPI_SLAVE_TX_PIN,
           WIZ_SPI_SLAVE_RX_PIN,  WIZ_SPI_SLAVE_CSN_PIN,
           WIZ_SPI_SLAVE_IRQ_PIN);
}

void wiz_spi_slave_poll(void)
{
    uint8_t b;

    if (!spi_is_readable(spi0)) { return; }
    b = (uint8_t)spi_get_hw(spi0)->dr;
    if (b != SPI_CLAW_MAGIC_0) { return; }

    _rx_bytes(&b, 1);
    if (b != SPI_CLAW_MAGIC_1) { return; }

    _handle_rx();
}

void wiz_spi_slave_send(spi_claw_cmd_t cmd, const uint8_t *payload, uint16_t len)
{
    if (len > SPI_CLAW_MAX_CHUNK) { return; }
    printf("[spi_slave] SEND cmd=0x%02X len=%u seq=%u\n", (unsigned)cmd, len, s_seq);

    spi_claw_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0] = SPI_CLAW_MAGIC_0;
    hdr.magic[1] = SPI_CLAW_MAGIC_1;
    hdr.cmd      = (uint8_t)cmd;
    hdr.len      = len;
    hdr.seq      = s_seq++;
    hdr.crc      = 0;
    hdr.crc      = _calc_crc((const uint8_t *)&hdr, payload, len);

    gpio_put(WIZ_SPI_SLAVE_IRQ_PIN, 1);

    spi_write_blocking(spi0, (const uint8_t *)&hdr, SPI_CLAW_HDR_SIZE);
    if (len > 0) {
        spi_write_blocking(spi0, payload, len);
    }

    gpio_put(WIZ_SPI_SLAVE_IRQ_PIN, 0);
    printf("[spi_slave] SEND done cmd=0x%02X\n", (unsigned)cmd);
}
