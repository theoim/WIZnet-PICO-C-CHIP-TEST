/*
 * wiz_claw_agent.h — Agent 루프 (LLM + tool 호출 반복)
 *
 * 사용자 텍스트 한 턴을 받아 LLM 호출 → tool_call 실행 → 재호출 루프를
 * max_tool_iterations 안에서 처리하고 최종 텍스트 응답을 반환한다.
 */
#pragma once

#include "wiz_claw.h"
#include "wiz_claw_llm.h"

/* ── tool 실행 콜백 ───────────────────────────────────────────
 *
 * name          : LLM이 요청한 함수 이름 (예: "led_control")
 * arguments_json: LLM이 넘긴 인자 JSON (예: {"state":"on"})
 * out_result_json: 실행 결과 JSON 문자열 (heap). 호출자가 free().
 *                 예: "{\"ok\":true}"
 */
typedef wiz_claw_err_t (*wiz_claw_tool_handler_fn)(
    const char  *name,
    const char  *arguments_json,
    char       **out_result_json,
    void        *user_ctx
);

/* ── Agent 설정 ───────────────────────────────────────────── */
typedef struct {
    wiz_claw_llm_config_t     llm;
    const char               *system_prompt;
    const char               *tools_json;           /* tool 정의 배열 JSON */
    wiz_claw_tool_handler_fn  tool_handler;
    void                     *tool_handler_ctx;
    uint32_t                  max_tool_iterations;  /* 0 → 기본값 10 */
} wiz_claw_agent_config_t;

/* ── API ─────────────────────────────────────────────────── */

/*
 * 한 턴 실행.
 *
 * session_messages : 대화 히스토리 cJSON 배열. 내부에서 user/assistant
 *                    메시지가 추가되며 다음 턴에 재사용할 수 있다.
 * user_text        : 이번 턴 사용자 입력.
 * out_reply        : 최종 텍스트 응답 (heap). 호출자가 free().
 * out_error_message: 실패 원인 문자열 (heap). 성공 시 NULL. 호출자가 free().
 */
wiz_claw_err_t wiz_claw_agent_run(
    const wiz_claw_agent_config_t *config,
    cJSON                         *session_messages,
    const char                    *user_text,
    char                         **out_reply,
    char                         **out_error_message
);
