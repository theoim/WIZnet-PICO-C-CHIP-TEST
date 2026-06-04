/*
 * wiz_claw_llm.c
 *
 * OpenAI 호환 LLM API 요청 빌드 / 응답 파싱 / 세션 헬퍼.
 * claw_llm_backend_openai_compatible.c 에서 JSON 처리 부분 추출.
 */
#include "wiz_claw_llm.h"

static const char *TAG = "wiz_claw_llm";

#define LLM_DEFAULT_MAX_TOKENS  4096
#define LLM_DEFAULT_CHAT_PATH   "/v1/chat/completions"

/* ── 내부: 요청 JSON 빌드 ────────────────────────────────────── */

static char *build_request_json(
    const wiz_claw_llm_config_t *cfg,
    const char                  *system_prompt,
    cJSON                       *messages,
    const char                  *tools_json,
    char                       **out_error)
{
    cJSON *body       = NULL;
    cJSON *msg_array  = NULL;
    cJSON *sys_msg    = NULL;
    cJSON *item       = NULL;
    char  *result     = NULL;
    uint32_t max_tok  = cfg->max_tokens ? cfg->max_tokens : LLM_DEFAULT_MAX_TOKENS;

    body      = cJSON_CreateObject();
    msg_array = cJSON_CreateArray();
    sys_msg   = cJSON_CreateObject();
    if (!body || !msg_array || !sys_msg) {
        *out_error = strdup("OOM building request");
        goto fail;
    }

    cJSON_AddStringToObject(body,    "model",      cfg->model);
    cJSON_AddNumberToObject(body,    "max_tokens",  (double)max_tok);
    cJSON_AddStringToObject(sys_msg, "role",       "system");
    cJSON_AddStringToObject(sys_msg, "content",    system_prompt);
    cJSON_AddItemToArray(msg_array, sys_msg);
    sys_msg = NULL;

    cJSON_ArrayForEach(item, messages) {
        cJSON *dup = cJSON_Duplicate(item, true);
        if (!dup) { *out_error = strdup("OOM duplicating messages"); goto fail; }
        cJSON_AddItemToArray(msg_array, dup);
    }

    cJSON_AddItemToObject(body, "messages", msg_array);
    msg_array = NULL;

    if (tools_json && tools_json[0] && cfg->supports_tools) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (!tools || !cJSON_IsArray(tools)) {
            cJSON_Delete(tools);
            *out_error = strdup("Invalid tools_json");
            goto fail;
        }
        cJSON_AddItemToObject(body, "tools", tools);
    }

    result = cJSON_PrintUnformatted(body);
    if (!result) { *out_error = strdup("OOM serializing request"); }

fail:
    cJSON_Delete(body);
    cJSON_Delete(msg_array);
    cJSON_Delete(sys_msg);
    return result;
}

/* ── 내부: 응답 JSON 파싱 ────────────────────────────────────── */

static wiz_claw_err_t parse_response_json(
    const char              *body,
    wiz_claw_llm_response_t *out,
    char                   **out_error)
{
    cJSON *root       = NULL;
    cJSON *choices    = NULL;
    cJSON *choice0    = NULL;
    cJSON *message    = NULL;
    cJSON *content    = NULL;
    cJSON *reasoning  = NULL;
    cJSON *tool_calls = NULL;
    cJSON *tc         = NULL;
    size_t tc_count   = 0;
    size_t tc_index   = 0;
    wiz_claw_err_t err = WIZ_CLAW_OK;

    memset(out, 0, sizeof(*out));

    root = cJSON_Parse(body);
    if (!root) { *out_error = strdup("JSON parse failed"); return WIZ_CLAW_ERR_JSON; }

    choices = cJSON_GetObjectItem(root, "choices");
    choice0 = (choices && cJSON_IsArray(choices)) ? cJSON_GetArrayItem(choices, 0) : NULL;
    message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;

    if (!message || !cJSON_IsObject(message)) {
        *out_error = strdup("LLM response missing message");
        err = WIZ_CLAW_ERR_JSON;
        goto done;
    }

    content = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content) && content->valuestring && content->valuestring[0]) {
        out->text = strdup(content->valuestring);
        if (!out->text) { err = WIZ_CLAW_ERR_NO_MEM; goto done; }
    }

    reasoning = cJSON_GetObjectItem(message, "reasoning_content");
    if (cJSON_IsString(reasoning) && reasoning->valuestring) {
        out->reasoning = strdup(reasoning->valuestring);
    }

    tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        tc_count = (size_t)cJSON_GetArraySize(tool_calls);
        if (tc_count > 0) {
            out->tool_calls = (wiz_claw_tool_call_t *)
                calloc(tc_count, sizeof(wiz_claw_tool_call_t));
            if (!out->tool_calls) { err = WIZ_CLAW_ERR_NO_MEM; goto done; }
            out->tool_call_count = tc_count;
        }

        cJSON_ArrayForEach(tc, tool_calls) {
            wiz_claw_tool_call_t *dst = &out->tool_calls[tc_index];
            cJSON *id_j   = cJSON_GetObjectItem(tc, "id");
            cJSON *func_j = cJSON_GetObjectItem(tc, "function");
            cJSON *name_j = func_j ? cJSON_GetObjectItem(func_j, "name")      : NULL;
            cJSON *args_j = func_j ? cJSON_GetObjectItem(func_j, "arguments") : NULL;

            if (!id_j || !name_j || !args_j) {
                *out_error = strdup("Malformed tool_call");
                err = WIZ_CLAW_ERR_JSON;
                goto done;
            }
            dst->id            = strdup(id_j->valuestring);
            dst->name          = strdup(name_j->valuestring);
            dst->arguments_json = strdup(args_j->valuestring);
            if (!dst->id || !dst->name || !dst->arguments_json) {
                err = WIZ_CLAW_ERR_NO_MEM;
                goto done;
            }
            tc_index++;
        }
    }

    if (!out->text && out->tool_call_count == 0) {
        *out_error = strdup("LLM returned empty response");
        err = WIZ_CLAW_ERR_JSON;
    }

done:
    cJSON_Delete(root);
    if (err != WIZ_CLAW_OK) {
        wiz_claw_llm_response_free(out);
    }
    return err;
}

/* ── wiz_claw_llm_chat ──────────────────────────────────────── */

wiz_claw_err_t wiz_claw_llm_chat(
    const wiz_claw_llm_config_t *config,
    const char                  *system_prompt,
    cJSON                       *messages,
    const char                  *tools_json,
    wiz_claw_llm_response_t     *out_response,
    char                       **out_error_message)
{
    char *body_json   = NULL;
    char *response    = NULL;
    char *url         = NULL;
    char *auth_header = NULL;
    wiz_claw_err_t err;

    if (!config || !system_prompt || !messages || !out_response || !out_error_message) {
        return WIZ_CLAW_ERR_INVALID_ARG;
    }
    *out_error_message = NULL;

    const char *chat_path = (config->chat_path && config->chat_path[0])
                                ? config->chat_path : LLM_DEFAULT_CHAT_PATH;

    url = wiz_claw_dup_printf("%s%s", config->base_url, chat_path);
    if (!url) { return WIZ_CLAW_ERR_NO_MEM; }

    auth_header = wiz_claw_dup_printf("Bearer %s", config->api_key);
    if (!auth_header) { free(url); return WIZ_CLAW_ERR_NO_MEM; }

    body_json = build_request_json(config, system_prompt, messages,
                                   tools_json, out_error_message);
    if (!body_json) {
        err = WIZ_CLAW_ERR_NO_MEM;
        goto done;
    }

    WIZ_CLAW_LOG_I(TAG, "POST %s model=%s", url, config->model);

    err = config->http_post(url, auth_header, body_json, &response,
                            config->http_user_ctx);
    if (err != WIZ_CLAW_OK || !response) {
        WIZ_CLAW_LOG_E(TAG, "HTTP POST failed: %d", err);
        if (!*out_error_message) {
            *out_error_message = strdup("HTTP request failed");
        }
        goto done;
    }

    err = parse_response_json(response, out_response, out_error_message);
    if (err == WIZ_CLAW_OK) {
        WIZ_CLAW_LOG_I(TAG, "response: text=%.48s tool_calls=%zu",
                        out_response->text ? out_response->text : "(none)",
                        out_response->tool_call_count);
    }

done:
    free(url);
    free(auth_header);
    free(body_json);
    free(response);
    return err;
}

/* ── wiz_claw_llm_response_free ─────────────────────────────── */

void wiz_claw_llm_response_free(wiz_claw_llm_response_t *resp)
{
    size_t i;
    if (!resp) { return; }

    free(resp->text);
    free(resp->reasoning);
    for (i = 0; i < resp->tool_call_count; i++) {
        free(resp->tool_calls[i].id);
        free(resp->tool_calls[i].name);
        free(resp->tool_calls[i].arguments_json);
    }
    free(resp->tool_calls);
    memset(resp, 0, sizeof(*resp));
}

/* ── 세션 헬퍼 ──────────────────────────────────────────────── */

cJSON *wiz_claw_session_create(void)
{
    return cJSON_CreateArray();
}

void wiz_claw_session_append_user(cJSON *messages, const char *text)
{
    cJSON *msg = cJSON_CreateObject();
    if (!msg) { return; }
    cJSON_AddStringToObject(msg, "role",    "user");
    cJSON_AddStringToObject(msg, "content", text);
    cJSON_AddItemToArray(messages, msg);
}

void wiz_claw_session_append_assistant_text(cJSON *messages, const char *text)
{
    cJSON *msg = cJSON_CreateObject();
    if (!msg) { return; }
    cJSON_AddStringToObject(msg, "role",    "assistant");
    cJSON_AddStringToObject(msg, "content", text ? text : "");
    cJSON_AddItemToArray(messages, msg);
}

void wiz_claw_session_append_assistant_tool_calls(
    cJSON *messages,
    const wiz_claw_llm_response_t *resp)
{
    size_t i;
    cJSON *msg        = cJSON_CreateObject();
    cJSON *tool_calls = cJSON_CreateArray();
    if (!msg || !tool_calls) { cJSON_Delete(msg); cJSON_Delete(tool_calls); return; }

    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddNullToObject(msg, "content");

    for (i = 0; i < resp->tool_call_count; i++) {
        const wiz_claw_tool_call_t *tc = &resp->tool_calls[i];
        cJSON *tc_obj   = cJSON_CreateObject();
        cJSON *func_obj = cJSON_CreateObject();
        if (!tc_obj || !func_obj) { cJSON_Delete(tc_obj); cJSON_Delete(func_obj); continue; }

        cJSON_AddStringToObject(tc_obj,   "id",   tc->id);
        cJSON_AddStringToObject(tc_obj,   "type", "function");
        cJSON_AddStringToObject(func_obj, "name", tc->name);

        /* arguments는 이미 JSON 문자열이므로 그대로 사용 */
        cJSON_AddStringToObject(func_obj, "arguments", tc->arguments_json);
        cJSON_AddItemToObject(tc_obj, "function", func_obj);
        cJSON_AddItemToArray(tool_calls, tc_obj);
    }

    cJSON_AddItemToObject(msg, "tool_calls", tool_calls);
    cJSON_AddItemToArray(messages, msg);
}

void wiz_claw_session_append_tool_result(
    cJSON      *messages,
    const char *tool_call_id,
    const char *result_json)
{
    cJSON *msg = cJSON_CreateObject();
    if (!msg) { return; }
    cJSON_AddStringToObject(msg, "role",         "tool");
    cJSON_AddStringToObject(msg, "tool_call_id", tool_call_id);
    cJSON_AddStringToObject(msg, "content",      result_json ? result_json : "{}");
    cJSON_AddItemToArray(messages, msg);
}
