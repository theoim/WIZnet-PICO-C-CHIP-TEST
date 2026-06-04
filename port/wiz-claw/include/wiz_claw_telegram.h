/*
 * wiz_claw_telegram.h — Telegram Bot API JSON 파싱 / 요청 빌드
 *
 * HTTP 전송은 wiz_claw_http_call_fn 콜백으로 주입.
 * TLS / 소켓은 이 모듈 밖의 책임.
 */
#pragma once

#include "wiz_claw.h"

/* ── HTTP 콜백 타입 ──────────────────────────────────────────
 *
 * url        : 완성된 URL (예: https://api.telegram.org/bot.../getUpdates)
 * body_json  : POST 바디. NULL이면 GET으로 처리해도 무방.
 * out_response: 응답 바디 (heap 할당). 호출자가 free().
 * user_ctx   : 콜백 등록 시 넘긴 컨텍스트 포인터.
 */
typedef wiz_claw_err_t (*wiz_claw_http_call_fn)(
    const char *url,
    const char *body_json,
    char      **out_response,
    void       *user_ctx
);

/* ── 수신 메시지 구조체 ───────────────────────────────────── */
typedef struct {
    char  chat_id[32];
    char  sender_id[32];
    char  message_id[32];
    char *text;           /* heap 할당 — wiz_claw_tg_message_free()로 해제 */
} wiz_claw_tg_message_t;

/* ── Telegram 클라이언트 설정 ─────────────────────────────── */
typedef struct {
    const char            *bot_token;
    const char            *api_base;   /* NULL → "https://api.telegram.org" */
    wiz_claw_http_call_fn  http_call;
    void                  *http_user_ctx;
} wiz_claw_tg_config_t;

/* ── 수신 콜백 ────────────────────────────────────────────── */
typedef void (*wiz_claw_tg_message_cb)(
    const wiz_claw_tg_message_t *msg,
    void                        *user_ctx
);

/* ── API ─────────────────────────────────────────────────── */

/*
 * getUpdates 응답 JSON을 파싱해 텍스트 메시지마다 callback을 호출한다.
 * inout_next_offset: 마지막 update_id+1 으로 갱신됨 (polling offset 유지용).
 */
wiz_claw_err_t wiz_claw_tg_parse_updates(
    const char             *response_json,
    int64_t                *inout_next_offset,
    wiz_claw_tg_message_cb  callback,
    void                   *user_ctx
);

/* getUpdates 호출 → 파싱 → callback 까지 한 번에 처리 */
wiz_claw_err_t wiz_claw_tg_poll(
    const wiz_claw_tg_config_t *config,
    int                         timeout_s,
    int64_t                    *inout_next_offset,
    wiz_claw_tg_message_cb      callback,
    void                       *user_ctx
);

/* sendMessage: chat_id 채팅방에 text 전송 */
wiz_claw_err_t wiz_claw_tg_send_text(
    const wiz_claw_tg_config_t *config,
    const char                 *chat_id,
    const char                 *text
);

/* 수신 메시지 해제 */
void wiz_claw_tg_message_free(wiz_claw_tg_message_t *msg);
