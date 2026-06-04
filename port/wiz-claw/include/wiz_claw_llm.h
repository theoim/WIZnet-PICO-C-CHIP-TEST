/*
 * wiz_claw_llm.h — LLM 요청 빌드 / 응답 파싱 (OpenAI 호환)
 *
 * Anthropic 백엔드는 별도 구현 가능. 현재는 OpenAI-compatible API 기준.
 * HTTP 전송은 wiz_claw_http_post_fn 콜백으로 주입.
 */
#pragma once

#include "wiz_claw.h"
#include "cJSON.h"

/* ── HTTP POST 콜백 ───────────────────────────────────────────
 *
 * url         : 완성된 엔드포인트 URL
 * auth_header : "Bearer sk-..."  형태의 Authorization 헤더 값
 * body_json   : 직렬화된 요청 JSON 문자열
 * out_response: 응답 바디 (heap 할당). 호출자가 free().
 */
typedef wiz_claw_err_t (*wiz_claw_http_post_fn)(
    const char *url,
    const char *auth_header,
    const char *body_json,
    char      **out_response,
    void       *user_ctx
);

/* ── tool_call 하나 ───────────────────────────────────────── */
typedef struct {
    char *id;
    char *name;
    char *arguments_json;
} wiz_claw_tool_call_t;

/* ── LLM 응답 ─────────────────────────────────────────────── */
typedef struct {
    char                *text;
    char                *reasoning;       /* DeepSeek reasoning_content 등 */
    wiz_claw_tool_call_t *tool_calls;
    size_t               tool_call_count;
} wiz_claw_llm_response_t;

/* ── LLM 클라이언트 설정 ──────────────────────────────────── */
typedef struct {
    const char           *base_url;       /* 예: "https://api.openai.com"   */
    const char           *api_key;
    const char           *model;          /* 예: "gpt-4o"                   */
    const char           *chat_path;      /* NULL → "/v1/chat/completions"  */
    uint32_t              max_tokens;     /* 0 → 기본값 4096 사용           */
    bool                  supports_tools;
    wiz_claw_http_post_fn http_post;
    void                 *http_user_ctx;
} wiz_claw_llm_config_t;

/* ── API ─────────────────────────────────────────────────── */

/*
 * chat 요청을 보내고 응답을 파싱한다.
 *
 * system_prompt : 시스템 프롬프트 문자열
 * messages      : cJSON 배열 (대화 히스토리). wiz_claw_session_* 헬퍼로 구성.
 * tools_json    : tool 정의 JSON 배열 문자열. NULL이면 tools 미포함.
 * out_response  : 파싱된 응답 구조체. wiz_claw_llm_response_free()로 해제.
 * out_error_message: 실패 시 원인 문자열 (heap). 성공 시 NULL. 호출자가 free().
 */
wiz_claw_err_t wiz_claw_llm_chat(
    const wiz_claw_llm_config_t *config,
    const char                  *system_prompt,
    cJSON                       *messages,
    const char                  *tools_json,
    wiz_claw_llm_response_t     *out_response,
    char                       **out_error_message
);

void wiz_claw_llm_response_free(wiz_claw_llm_response_t *resp);

/* ── 세션(대화 히스토리) 헬퍼 ─────────────────────────────── */

/* 빈 messages 배열 생성 */
cJSON *wiz_claw_session_create(void);

/* user 메시지 추가 */
void wiz_claw_session_append_user(cJSON *messages, const char *text);

/* assistant 텍스트 응답 추가 */
void wiz_claw_session_append_assistant_text(cJSON *messages, const char *text);

/* assistant tool_calls 메시지 추가 (tool 호출 기록용) */
void wiz_claw_session_append_assistant_tool_calls(
    cJSON *messages,
    const wiz_claw_llm_response_t *resp
);

/* tool 실행 결과 추가 */
void wiz_claw_session_append_tool_result(
    cJSON      *messages,
    const char *tool_call_id,
    const char *result_json
);
