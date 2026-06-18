/* wiz_claw_spi_proto.h — ESP32 ↔ RP2040 공통 SPI 프로토콜 헤더 */
#ifndef WIZ_CLAW_SPI_PROTO_H
#define WIZ_CLAW_SPI_PROTO_H

#include <stdint.h>

#define SPI_CLAW_MAGIC_0   0xCA
#define SPI_CLAW_MAGIC_1   0xFE
#define SPI_CLAW_MAX_CHUNK 4096u   /* 단일 패킷 최대 페이로드 */
#define SPI_CLAW_HDR_SIZE  7u      /* sizeof(spi_claw_hdr_t) */

typedef enum {
    /* ── W55RP20 → ESP32 (다운스트림) ───────────── */
    SPI_CMD_PING         = 0x01,
    SPI_CMD_LUA_EXEC     = 0x02,  /* payload: Lua 스크립트 문자열 */
    SPI_CMD_GPIO_SET     = 0x03,  /* payload: {"pin":N,"state":"on|off|toggle"} */
    SPI_CMD_CAPTURE_REQ  = 0x04,  /* payload: 없음 (캡처 후 CHUNK 전송 요청) */
    SPI_CMD_LLM_REQ      = 0x05,  /* payload: {"session_id":"tg_<chat_id>","chat_id":"...","sender":"...","text":"..."} */

    /* ── ESP32 → W55RP20 (업스트림) ─────────────── */
    SPI_CMD_EVENT        = 0x40,  /* payload: {"type":"motion","label":"cat",...} */
    SPI_CMD_CHUNK_DATA   = 0x41,  /* payload: {"seq":N,"total":T,"data":[...]} */
    SPI_CMD_CHUNK_END    = 0x42,  /* payload: {"mime":"image/jpeg","size":N} */
    SPI_CMD_LLM_RESP     = 0x43,  /* payload: {"session_id":"...","ok":true,"text":"..."} */
    SPI_CMD_ESP_STATUS   = 0x44,  /* payload: {"wifi":true,"agent":true} */

    /* ── 양방향 ──────────────────────────────────── */
    SPI_CMD_ACK          = 0x80,  /* payload: {"seq":N,"ok":true} */
    SPI_CMD_NACK         = 0x81,  /* payload: {"seq":N,"error":"..."} */
    SPI_CMD_PONG         = 0x82,  /* CMD_PING 응답 */
} spi_claw_cmd_t;

/* 고정 7바이트 헤더 — 양쪽 동일 구조체 */
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];  /* SPI_CLAW_MAGIC_0, SPI_CLAW_MAGIC_1 */
    uint8_t  cmd;       /* spi_claw_cmd_t */
    uint16_t len;       /* payload 길이, little-endian */
    uint8_t  seq;       /* 시퀀스 번호 (ACK 매칭) */
    uint8_t  crc;       /* 헤더+페이로드 XOR 체크섬 (이 필드는 0으로 두고 계산) */
} spi_claw_hdr_t;

#endif /* WIZ_CLAW_SPI_PROTO_H */
