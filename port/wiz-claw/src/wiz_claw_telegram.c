/*
 * wiz_claw_telegram.c
 *
 * Telegram getUpdates 파싱 / sendMessage 빌드.
 * cap_im_tg.c의 JSON 처리 부분만 추출, ESP-IDF 의존성 제거.
 */
#include "wiz_claw_telegram.h"
#include "cJSON.h"

#include <inttypes.h>

static const char *TAG = "wiz_claw_tg";

#define TG_API_BASE_DEFAULT "https://api.telegram.org"
#define TG_URL_BUF_SIZE     256

/* ── 내부 유틸 ──────────────────────────────────────────────── */

static const char *tg_api_base(const wiz_claw_tg_config_t *cfg)
{
    return (cfg->api_base && cfg->api_base[0]) ? cfg->api_base : TG_API_BASE_DEFAULT;
}

static char *tg_build_url(const wiz_claw_tg_config_t *cfg, const char *method)
{
    return wiz_claw_dup_printf("%s/bot%s/%s",
                               tg_api_base(cfg), cfg->bot_token, method);
}

/* ── wiz_claw_tg_message_free ───────────────────────────────── */

void wiz_claw_tg_message_free(wiz_claw_tg_message_t *msg)
{
    if (!msg) {
        return;
    }
    free(msg->text);
    msg->text = NULL;
}

/* ── wiz_claw_tg_parse_updates ──────────────────────────────── */

wiz_claw_err_t wiz_claw_tg_parse_updates(
    const char             *response_json,
    int64_t                *inout_next_offset,
    wiz_claw_tg_message_cb  callback,
    void                   *user_ctx)
{
    cJSON *root      = NULL;
    cJSON *ok_json   = NULL;
    cJSON *result    = NULL;
    cJSON *update    = NULL;

    if (!response_json || !inout_next_offset || !callback) {
        return WIZ_CLAW_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(response_json);
    if (!root) {
        WIZ_CLAW_LOG_E(TAG, "JSON parse failed");
        return WIZ_CLAW_ERR_JSON;
    }

    ok_json = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok_json)) {
        WIZ_CLAW_LOG_E(TAG, "Telegram API returned ok=false");
        cJSON_Delete(root);
        return WIZ_CLAW_ERR_HTTP;
    }

    result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return WIZ_CLAW_ERR_JSON;
    }

    cJSON_ArrayForEach(update, result) {
        cJSON *update_id_json = cJSON_GetObjectItem(update, "update_id");
        cJSON *message        = cJSON_GetObjectItem(update, "message");
        cJSON *chat           = NULL;
        cJSON *from           = NULL;
        cJSON *text_json      = NULL;
        int64_t update_id;

        if (!cJSON_IsNumber(update_id_json) || !cJSON_IsObject(message)) {
            continue;
        }

        update_id = (int64_t)update_id_json->valuedouble;
        if (update_id >= *inout_next_offset) {
            *inout_next_offset = update_id + 1;
        }

        text_json = cJSON_GetObjectItem(message, "text");
        if (!cJSON_IsString(text_json) || !text_json->valuestring[0]) {
            continue;
        }

        chat = cJSON_GetObjectItem(message, "chat");
        from = cJSON_GetObjectItem(message, "from");

        wiz_claw_tg_message_t msg = {0};
        cJSON *chat_id_json    = cJSON_GetObjectItem(chat, "id");
        cJSON *from_id_json    = cJSON_IsObject(from) ?
                                     cJSON_GetObjectItem(from, "id") : NULL;
        cJSON *message_id_json = cJSON_GetObjectItem(message, "message_id");

        if (!cJSON_IsNumber(chat_id_json)) {
            continue;
        }

        snprintf(msg.chat_id,    sizeof(msg.chat_id),
                 "%" PRId64, (int64_t)chat_id_json->valuedouble);
        snprintf(msg.message_id, sizeof(msg.message_id),
                 "%" PRId64,
                 cJSON_IsNumber(message_id_json) ?
                     (int64_t)message_id_json->valuedouble : 0LL);
        if (cJSON_IsNumber(from_id_json)) {
            snprintf(msg.sender_id, sizeof(msg.sender_id),
                     "%" PRId64, (int64_t)from_id_json->valuedouble);
        }

        msg.text = strdup(text_json->valuestring);
        if (!msg.text) {
            WIZ_CLAW_LOG_E(TAG, "OOM copying message text");
            continue;
        }

        callback(&msg, user_ctx);
        wiz_claw_tg_message_free(&msg);
    }

    cJSON_Delete(root);
    return WIZ_CLAW_OK;
}

/* ── wiz_claw_tg_poll ───────────────────────────────────────── */

wiz_claw_err_t wiz_claw_tg_poll(
    const wiz_claw_tg_config_t *config,
    int                         timeout_s,
    int64_t                    *inout_next_offset,
    wiz_claw_tg_message_cb      callback,
    void                       *user_ctx)
{
    char *url      = NULL;
    char *body     = NULL;
    char *response = NULL;
    wiz_claw_err_t err;
    cJSON *req = NULL;

    if (!config || !config->bot_token || !config->http_call || !inout_next_offset) {
        return WIZ_CLAW_ERR_INVALID_ARG;
    }

    url = tg_build_url(config, "getUpdates");
    if (!url) {
        return WIZ_CLAW_ERR_NO_MEM;
    }

    req = cJSON_CreateObject();
    if (!req) { free(url); return WIZ_CLAW_ERR_NO_MEM; }

    cJSON_AddNumberToObject(req, "timeout", timeout_s);
    cJSON_AddNumberToObject(req, "offset",  (double)*inout_next_offset);
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    if (!body) { free(url); return WIZ_CLAW_ERR_NO_MEM; }

    WIZ_CLAW_LOG_I(TAG, "polling offset=%" PRId64, *inout_next_offset);

    err = config->http_call(url, body, &response, config->http_user_ctx);
    free(url);
    free(body);

    if (err != WIZ_CLAW_OK || !response) {
        WIZ_CLAW_LOG_E(TAG, "getUpdates HTTP failed: %d", err);
        free(response);
        return err;
    }

    err = wiz_claw_tg_parse_updates(response, inout_next_offset, callback, user_ctx);
    free(response);
    return err;
}

/* ── wiz_claw_tg_send_text ──────────────────────────────────── */

wiz_claw_err_t wiz_claw_tg_send_text(
    const wiz_claw_tg_config_t *config,
    const char                 *chat_id,
    const char                 *text)
{
    char *url      = NULL;
    char *body     = NULL;
    char *response = NULL;
    wiz_claw_err_t err;
    cJSON *req = NULL;

    if (!config || !config->bot_token || !config->http_call || !chat_id || !text) {
        return WIZ_CLAW_ERR_INVALID_ARG;
    }

    url = tg_build_url(config, "sendMessage");
    if (!url) { return WIZ_CLAW_ERR_NO_MEM; }

    req = cJSON_CreateObject();
    if (!req) { free(url); return WIZ_CLAW_ERR_NO_MEM; }

    cJSON_AddStringToObject(req, "chat_id", chat_id);
    cJSON_AddStringToObject(req, "text",    text);
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    if (!body) { free(url); return WIZ_CLAW_ERR_NO_MEM; }

    err = config->http_call(url, body, &response, config->http_user_ctx);
    free(url);
    free(body);
    free(response);

    if (err != WIZ_CLAW_OK) {
        WIZ_CLAW_LOG_E(TAG, "sendMessage failed: %d", err);
    }
    return err;
}
