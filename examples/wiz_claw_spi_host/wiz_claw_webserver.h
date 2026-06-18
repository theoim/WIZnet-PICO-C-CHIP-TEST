/*
 * wiz_claw_webserver.h — W55RP20 설정 웹서버 (HTTP, 소켓 7, 포트 80)
 *
 * 설정 구조체를 flash 마지막 sector에 저장.
 * Core 0 메인 루프에서 wiz_claw_webserver_poll() 호출.
 * Flash 쓰기 시 multicore_lockout_victim_init()이 Core 1에서
 * 먼저 호출되어 있어야 함.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define WIZ_CLAW_WS_MAGIC  0x57435753u   /* 'W','C','W','S' */
#define WIZ_CLAW_WS_SOCK   7
#define WIZ_CLAW_WS_PORT   80

/*
 * 1024 bytes = 4 × FLASH_PAGE_SIZE(256).
 * flash_range_program() 정렬 요건 충족.
 * 필드 수정 시 sizeof == 1024 static_assert 통과 여부 확인.
 */
typedef struct {
    uint32_t magic;
    /* ── Network ─────────────────────────────────────────────── */
    uint8_t  ip[4];
    uint8_t  gw[4];
    uint8_t  sn[4];
    uint8_t  dns[4];
    uint8_t  dhcp;          /* 0 = static, 1 = DHCP */
    uint8_t  _pad0[3];
    /* ── LLM ─────────────────────────────────────────────────── */
    char     llm_api_key[128];
    char     llm_base_url[128];
    char     llm_model[64];
    char     llm_chat_path[128];
    char     llm_max_tokens[8];
    /* ── Telegram ─────────────────────────────────────────────── */
    char     tg_bot_token[64];
    /* ── Vision API ───────────────────────────────────────────── */
    char     vision_host[128];
    char     vision_path[128];
    /* ── Pad to 1024 bytes ────────────────────────────────────── */
    uint8_t  _pad1[224];
} wiz_claw_settings_t;

/* Flash에서 읽기. magic 불일치 시 기본값 채움 */
void wiz_claw_settings_load(wiz_claw_settings_t *out);

/* Flash에 저장. Core 1이 multicore_lockout_victim_init() 완료 후 호출 */
bool wiz_claw_settings_save(const wiz_claw_settings_t *cfg);

/* 웹서버 초기화. 네트워크 up 이후, 메인루프 직전에 한 번 호출 */
void wiz_claw_webserver_init(const wiz_claw_settings_t *settings);

/* 비블로킹 poll — Core 0 메인루프에서 매 iteration 호출 */
void wiz_claw_webserver_poll(void);
