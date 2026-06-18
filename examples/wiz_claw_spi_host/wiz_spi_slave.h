#ifndef WIZ_SPI_SLAVE_H
#define WIZ_SPI_SLAVE_H

#include "wiz_claw_spi_proto.h"

/* ── 핀 할당 (SPI0, W6300 QSPI GP15-GP22 간섭 회피) ─────────────────
 *   SPI0_SCK  → GP2
 *   SPI0_TX   → GP3   (RP2350 TX = ESP32 MISO)
 *   SPI0_RX   → GP0   (RP2350 RX = ESP32 MOSI)
 *   SPI0_CSn  → GP1
 *   IRQ out   → GP4   (Pico→ESP32 데이터준비 신호)
 *
 * ESP32 결선:
 *   D8  (GPIO7  SCK)  → Pico GP2
 *   D9  (GPIO8  MISO) ← Pico GP3
 *   D10 (GPIO9  MOSI) → Pico GP0
 *   D0  (GPIO1  CS)   → Pico GP1
 *   D1  (GPIO2  IRQ)  ← Pico GP4
 */
#define WIZ_SPI_SLAVE_SCK_PIN  2
#define WIZ_SPI_SLAVE_TX_PIN   3
#define WIZ_SPI_SLAVE_RX_PIN   0
#define WIZ_SPI_SLAVE_CSN_PIN  1
#define WIZ_SPI_SLAVE_IRQ_PIN  4

/* 수신 콜백 — 유효한 패킷이 도착할 때마다 호출 */
typedef void (*wiz_spi_slave_rx_cb_t)(spi_claw_cmd_t cmd,
                                       uint8_t        seq,
                                       const uint8_t *payload,
                                       uint16_t       len,
                                       void          *user_ctx);

void wiz_spi_slave_init(wiz_spi_slave_rx_cb_t rx_cb, void *user_ctx);
void wiz_spi_slave_poll(void);
void wiz_spi_slave_send(spi_claw_cmd_t cmd, const uint8_t *payload, uint16_t len);

#endif /* WIZ_SPI_SLAVE_H */
