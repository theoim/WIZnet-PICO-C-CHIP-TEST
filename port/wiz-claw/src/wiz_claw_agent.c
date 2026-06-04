/*
 * wiz_claw_agent.c
 *
 * LLM 호출 → tool_call 실행 → 재호출 루프.
 * claw_core.c의 agent 루프 로직을 동기(blocking) 버전으로 단순화.
 * FreeRTOS / 스레딩은 이 모듈 밖(호출자)에서 처리.
 */
#include "wiz_claw_agent.h"

static const char *TAG = "wiz_claw_agent";

#define AGENT_DEFAULT_MAX_ITERATIONS 10

wiz_claw_err_t wiz_claw_agent_run(
    const wiz_claw_agent_config_t *config,
    cJSON                         *session_messages,
    const char                    *user_text,
    char                         **out_reply,
    char                         **out_error_message)
{
    wiz_claw_llm_response_t resp = {0};
    wiz_claw_err_t err;
    uint32_t iterations = 0;
    uint32_t max_iter;
    size_t   i;

    if (!config || !session_messages || !user_text || !out_reply || !out_error_message) {
        return WIZ_CLAW_ERR_INVALID_ARG;
    }

    *out_reply         = NULL;
    *out_error_message = NULL;

    max_iter = config->max_tool_iterations ? config->max_tool_iterations
                                           : AGENT_DEFAULT_MAX_ITERATIONS;

    /* 사용자 메시지를 히스토리에 추가 */
    wiz_claw_session_append_user(session_messages, user_text);

    while (iterations < max_iter) {
        iterations++;

        WIZ_CLAW_LOG_I(TAG, "LLM call iteration=%u", iterations);

        err = wiz_claw_llm_chat(
            &config->llm,
            config->system_prompt,
            session_messages,
            config->tools_json,
            &resp,
            out_error_message);

        if (err != WIZ_CLAW_OK) {
            WIZ_CLAW_LOG_E(TAG, "LLM chat failed: %d msg=%s",
                           err, *out_error_message ? *out_error_message : "");
            return err;
        }

        /* tool_call이 없으면 텍스트 응답으로 종료 */
        if (resp.tool_call_count == 0) {
            wiz_claw_session_append_assistant_text(session_messages, resp.text);
            *out_reply = resp.text;
            resp.text  = NULL;
            wiz_claw_llm_response_free(&resp);
            return WIZ_CLAW_OK;
        }

        /* tool_call 처리 */
        WIZ_CLAW_LOG_I(TAG, "tool_calls=%zu", resp.tool_call_count);
        wiz_claw_session_append_assistant_tool_calls(session_messages, &resp);

        for (i = 0; i < resp.tool_call_count; i++) {
            const wiz_claw_tool_call_t *tc = &resp.tool_calls[i];
            char *result_json = NULL;

            WIZ_CLAW_LOG_I(TAG, "tool=%s args=%.64s", tc->name, tc->arguments_json);

            if (config->tool_handler) {
                err = config->tool_handler(tc->name, tc->arguments_json,
                                           &result_json, config->tool_handler_ctx);
                if (err != WIZ_CLAW_OK) {
                    WIZ_CLAW_LOG_W(TAG, "tool handler failed: %d", err);
                    /* 실패해도 루프 계속: LLM에 에러 결과를 돌려준다 */
                    free(result_json);
                    result_json = strdup("{\"error\":\"tool execution failed\"}");
                }
            } else {
                result_json = strdup("{\"error\":\"no tool handler registered\"}");
            }

            wiz_claw_session_append_tool_result(session_messages,
                                                tc->id,
                                                result_json);
            free(result_json);
        }

        wiz_claw_llm_response_free(&resp);
    }

    /* max_iter 초과 */
    WIZ_CLAW_LOG_W(TAG, "max tool iterations (%u) reached", max_iter);
    *out_error_message = strdup("max tool iterations exceeded");
    return WIZ_CLAW_ERR_FAIL;
}
