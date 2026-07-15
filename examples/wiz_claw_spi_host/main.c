/**
 * wiz_claw_spi_host — RP2350 + WIZnet W55RP20 SPI 코프로세서 예제
 *
 * Phase 2: 커맨드 실행
 *   - set_gpio  : SPI_CMD_GPIO_SET → ESP32 GPIO 제어 → ACK 수신
 *   - exec_lua  : SPI_CMD_LUA_EXEC → ESP32 Lua 실행 → ACK 수신
 *   - Core 0: Telegram 폴링 + LLM 에이전트
 *   - Core 1: SPI slave 폴링 (wiz_spi_slave_poll)
 *
 * 설정:
 *   아래 #define 세 줄만 채우면 됩니다.
 */

/* ── 사용자 설정은 웹 대시보드에서 관리 ────────────────────────
 * http://<pico-ip>/  접속 후 저장 → 자동 리부트.
 * 최초 부팅 시 기본 IP: 192.168.11.20
 * ──────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <malloc.h>
 
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"

#include "wiz_claw_net.h"
#include "wiz_claw_http.h"
#include "wiz_claw_telegram.h"
#include "wiz_claw_tls.h"
#include "wiz_claw_llm.h"
#include "wiz_claw_agent.h"
#include "mbedtls/base64.h"

#include "wiz_spi_slave.h"
#include "wiz_claw_webserver.h"

/* ── 상수 ──────────────────────────────────────────────────────── */
#define LED_PIN             PICO_DEFAULT_LED_PIN
#define TG_POLL_TIMEOUT_SEC 5
#define AGENT_MAX_ITER      8

/* ── 사진 조립 버퍼 (Core0/Core1 공유) ─────────────────────────────
 * Core1이 CHUNK_DATA를 여기 누적, Core0이 CHUNK_END 신호를 감지해 sendPhoto.
 * JPEG 640x480 ≈ 30-80KB. 200KB면 충분. */
#define PHOTO_BUF_MAX        (200u * 1024u)
static uint8_t         *g_photo_buf       = NULL;
static size_t           g_photo_size      = 0;
static volatile bool    g_photo_ready     = false;
static volatile bool    g_photo_capturing = false;
static char             g_photo_mime[16]  = "image/jpeg";
static char             g_chat_id[32]     = "";   /* on_telegram_message 에서 갱신 */

static volatile bool    g_pir_triggered   = false;
static absolute_time_t  g_pir_cooldown_until = {0};  /* {0} = already past at boot */

/* ── ESP32 CLAW LLM 릴레이 (Core0 ↔ Core1 공유) ────────────────────
 * Core0: Telegram 메시지 → SPI_CMD_LLM_REQ → 대기
 * Core1: SPI_CMD_LLM_RESP 수신 → g_llm_resp_* 채우기 → g_llm_resp_ready=true
 * 타임아웃 시 로컬 Groq fallback.
 */
#define LLM_RESP_TEXT_MAX    3800u
#define LLM_RELAY_TIMEOUT_MS 90000u

/* ── HTTP 프록시 (Core1 수신 → Core0 처리) ──────────────────────────
 * Core1: HTTP_REQ/BODY/BODY_END 수신 → g_proxy_* 채우기 → g_proxy_ready=true
 * Core0: LLM_REQ 대기 루프에서 g_proxy_ready 감지 → HTTP POST → HTTP_RESP 전송
 */
#define HTTP_PROXY_URL_MAX   256u
#define HTTP_PROXY_AUTH_MAX  640u
#define HTTP_PROXY_BODY_MAX  (32u * 1024u)

static char          g_proxy_url[HTTP_PROXY_URL_MAX];
static char          g_proxy_auth[HTTP_PROXY_AUTH_MAX];
static uint8_t      *g_proxy_body     = NULL;
static uint32_t      g_proxy_body_len = 0;
static uint32_t      g_proxy_body_rcv = 0;
static volatile bool g_proxy_ready    = false;
static bool          g_proxy_crc_err  = false;  /* CRC error during body recv; send 500 on BODY_END */

static spin_lock_t     *g_llm_spin        = NULL;
static volatile bool    g_llm_resp_ready  = false;
static volatile bool    g_llm_resp_ok     = false;  /* true=success, false=error from ESP32 */
static char             g_llm_resp_text[LLM_RESP_TEXT_MAX + 1] = "";
static char             g_llm_resp_session[64] = "";
static volatile bool    g_esp32_alive     = false;  /* true after first successful PONG */

/* ── S-track stability instrumentation ─────────────────────────────
 * Relay outcome counters + periodic health line for soak/leak detection.
 * FreeHeap should stay flat across a 24h soak (no monotonic decline).
 */
static volatile uint32_t g_stat_relay_ok      = 0;
static volatile uint32_t g_stat_relay_err     = 0;  /* ESP32 returned ok=false */
static volatile uint32_t g_stat_relay_timeout = 0;  /* 90s deadline hit        */
static volatile uint32_t g_stat_local_fb      = 0;  /* local Groq fallback used */
static volatile uint32_t g_stat_local_fb_unavail = 0; /* local fallback skipped: no API key configured */
static volatile uint32_t g_esp32_last_seen_ms = 0;   /* last PING/STATUS rx time (P-5) */

/* ── Heartbeat: last ESP health snapshot from SPI_CMD_ESP_STATUS ──────
 * ESP now reports objective health (seq/up/heap/wifi/proxy) instead of a
 * hardcoded string. Guardian derives a 3-state liveness from it + the
 * relay-timeout streak. Spec: docs/HEARTBEAT.md (to be written). */
static volatile bool     g_esp_valid       = false;
static volatile uint32_t g_esp_seq         = 0;
static volatile uint32_t g_esp_up_s        = 0;
static volatile uint32_t g_esp_heap        = 0;
static volatile bool     g_esp_wifi        = false;
static volatile bool     g_esp_proxy       = false;
static volatile uint32_t g_relay_to_streak = 0;  /* consecutive relay timeouts */

/* Provisional threshold — tune after observing ESP heap baseline on soak.
 * Below this the guardian flags DEGRADED (OOM precursor, DEVLOG 10/11th) but
 * does NOT act yet. */
#define ESP_HEAP_FLOOR_BYTES   30000u
#define RELAY_HUNG_STREAK      3u

/* minimal JSON scalar extraction for the small, trusted status payload */
static bool esp_js_uint(const char *js, const char *key, uint32_t *out)
{
    char pat[16];
    int m = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (m <= 0 || m >= (int)sizeof(pat)) { return false; }
    const char *p = strstr(js, pat);
    if (!p) { return false; }
    *out = (uint32_t)strtoul(p + m, NULL, 10);
    return true;
}
static bool esp_js_bool_true(const char *js, const char *key)
{
    char pat[16];
    int m = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (m <= 0 || m >= (int)sizeof(pat)) { return false; }
    const char *p = strstr(js, pat);
    return p && strncmp(p + m, "true", 4) == 0;
}

/* Guardian liveness state (most-severe first). HUNG = STATUS still arriving
 * but agent not answering relays; DEAD = STATUS stopped (P-5). */
typedef enum { GUARD_DEAD, GUARD_HUNG, GUARD_DEGRADED, GUARD_HEALTHY } guard_state_t;
static guard_state_t guardian_state(void)
{
    if (!g_esp32_alive)                          { return GUARD_DEAD; }
    if (g_relay_to_streak >= RELAY_HUNG_STREAK)  { return GUARD_HUNG; }
    if (g_esp_valid && g_esp_heap && g_esp_heap < ESP_HEAP_FLOOR_BYTES)
                                                 { return GUARD_DEGRADED; }
    return GUARD_HEALTHY;
}
static const char *guard_state_str(guard_state_t s)
{
    switch (s) {
    case GUARD_DEAD:     return "DEAD";
    case GUARD_HUNG:     return "HUNG";
    case GUARD_DEGRADED: return "DEGRADED";
    default:             return "HEALTHY";
    }
}

extern char __StackLimit;
extern char __bss_end__;
static uint32_t free_heap_bytes(void)
{
    struct mallinfo m = mallinfo();
    return (uint32_t)(&__StackLimit - &__bss_end__) - (uint32_t)m.uordblks;
}

static void print_health_line(void)
{
    printf("[health] up=%lus heap=%lu esp=%s alive=%d "
           "relay_ok=%lu err=%lu to=%lu(streak=%lu) local_fb=%lu unavail=%lu | "
           "esp_seq=%lu esp_up=%lus esp_heap=%lu esp_wifi=%d\n",
           (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000u),
           (unsigned long)free_heap_bytes(),
           guard_state_str(guardian_state()),
           (int)g_esp32_alive,
           (unsigned long)g_stat_relay_ok,
           (unsigned long)g_stat_relay_err,
           (unsigned long)g_stat_relay_timeout,
           (unsigned long)g_relay_to_streak,
           (unsigned long)g_stat_local_fb,
           (unsigned long)g_stat_local_fb_unavail,
           (unsigned long)g_esp_seq,
           (unsigned long)g_esp_up_s,
           (unsigned long)g_esp_heap,
           (int)g_esp_wifi);
}

/* ── P-5: dead-ESP detection ─────────────────────────────────────────
 * ESP sends SPI_CMD_PING (until first PONG) then SPI_CMD_ESP_STATUS every
 * 20s (see spi_status_task in edge_agent/main.c). g_esp32_alive used to
 * latch true forever once set (S-track scenario 4, DEVLOG 15th entry):
 * on independent power, an ESP that dies while the Pico stays up would
 * make every relay attempt burn the full 90s LLM_RELAY_TIMEOUT_MS before
 * falling back locally. This checks for 3 missed STATUS cycles (60s) with
 * a small margin and clears the flag so relay is skipped immediately.
 */
#define ESP32_ALIVE_TIMEOUT_MS (65u * 1000u)

static void check_esp32_alive_timeout(void)
{
    if (!g_esp32_alive) {
        return;
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((uint32_t)(now - g_esp32_last_seen_ms) > ESP32_ALIVE_TIMEOUT_MS) {
        g_esp32_alive = false;
        printf("[health] ESP32 STATUS timeout (%lums) — relay disabled, "
               "falling back to local until PING/STATUS resumes\n",
               (unsigned long)ESP32_ALIVE_TIMEOUT_MS);
    }
}

/* ── 소켓 할당 ─────────────────────────────────────────────────────
 *   0 : Telegram getUpdates (long-poll)
 *   1 : Telegram sendMessage
 *   2 : LLM API
 *   5 : DNS (UDP)
 *   6 : DHCP (UDP)
 */
static wiz_claw_http_ctx_t g_tg_poll_ctx  = { .socket_no = 0, .timeout_ms = 60000 };
static wiz_claw_http_ctx_t g_tg_send_ctx  = { .socket_no = 1, .timeout_ms = 30000 };
static wiz_claw_http_ctx_t g_llm_ctx      = { .socket_no = 2, .timeout_ms = 60000 };
static wiz_claw_http_ctx_t g_photo_ctx    = { .socket_no = 3, .timeout_ms = 60000 };
static wiz_claw_http_ctx_t g_vision_ctx   = { .socket_no = 4, .timeout_ms = 60000 };

/* global so capture_photo_tool_handler can send analysis message */
static wiz_claw_tg_config_t g_tg_send_cfg;

/* ── 전역 설정 (flash 로드, 웹서버에서 관리) ─────────────────── */
wiz_claw_settings_t g_settings;

/* 부팅 후 g_settings에서 채워짐 */
static wiz_NetInfo g_net_info;

/* ── 1초 타이머 ─────────────────────────────────────────────────── */
static struct repeating_timer g_1s_timer;

static bool timer_1s_cb(struct repeating_timer *t)
{
    (void)t;
    wiz_claw_net_1s_tick();
    return true;
}

/* ── set_gpio 툴 핸들러 ───────────────────────────────────────────
 *
 * Phase 2: ESP32에 SPI_CMD_GPIO_SET 포워드.
 * arguments JSON: {"pin":N,"state":"on|off|toggle"}
 */
static wiz_claw_err_t gpio_tool_handler(const char  *name,
                                         const char  *arguments_json,
                                         char       **out_result_json,
                                         void        *user_ctx)
{
    (void)user_ctx;
    (void)name;

    int pin = -1;
    const char *state = "toggle";

    const char *p_pin = strstr(arguments_json, "\"pin\"");
    if (p_pin) {
        p_pin = strchr(p_pin, ':');
        if (p_pin) { pin = atoi(p_pin + 1); }
    }
    const char *p_state = strstr(arguments_json, "\"state\"");
    if (p_state) {
        if (strstr(p_state, "\"on\""))       state = "on";
        else if (strstr(p_state, "\"off\"")) state = "off";
        else                                  state = "toggle";
    }

    char fwd[64];
    int  fwd_len = snprintf(fwd, sizeof(fwd),
                            "{\"pin\":%d,\"state\":\"%s\"}", pin, state);
    if (fwd_len > 0) {
        wiz_spi_slave_send(SPI_CMD_GPIO_SET,
                           (const uint8_t *)fwd, (uint16_t)fwd_len);
        printf("[tool] set_gpio pin=%d state=%s → ESP32\n", pin, state);
    }

    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"pin\":%d,\"state\":\"%s\",\"forwarded\":true}", pin, state);
    *out_result_json = strdup(buf);
    return WIZ_CLAW_OK;
}

/* ── Telegram sendPhoto (TLS multipart) ──────────────────────────
 *
 * JPEG 바이너리를 multipart/form-data POST로 Telegram에 전송.
 * wiz_claw_tls_* 저수준 API 직접 사용 (JSON이 아닌 바이너리 바디).
 */
#define TG_HOST "api.telegram.org"
#define TG_PORT  443

static wiz_claw_err_t send_photo_to_telegram(const char    *chat_id,
                                               const uint8_t *jpeg,
                                               size_t         jpeg_size)
{
    static const char SUFFIX[] = "\r\n--WizClawBnd--\r\n";
    const size_t slen = sizeof(SUFFIX) - 1;

    char prefix[384];
    int plen = snprintf(prefix, sizeof(prefix),
        "--WizClawBnd\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n"
        "\r\n"
        "%s\r\n"
        "--WizClawBnd\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n",
        chat_id);
    if (plen <= 0 || (size_t)plen >= sizeof(prefix)) { return WIZ_CLAW_ERR_INVALID_ARG; }

    size_t body_total = (size_t)plen + jpeg_size + slen;

    char req[768];
    int rlen = snprintf(req, sizeof(req),
        "POST /bot%s/sendPhoto HTTP/1.1\r\n"
        "Host: " TG_HOST "\r\n"
        "Content-Type: multipart/form-data; boundary=WizClawBnd\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        g_settings.tg_bot_token,
        (unsigned)body_total);
    if (rlen <= 0 || (size_t)rlen >= sizeof(req)) { return WIZ_CLAW_ERR_INVALID_ARG; }

    uint8_t ip[4];
    if (wiz_claw_net_dns_resolve(TG_HOST, ip) != WIZ_CLAW_OK) {
        printf("[sendPhoto] DNS failed\n");
        return WIZ_CLAW_ERR_FAIL;
    }

    static wiz_claw_tls_t s_photo_tls;
    wiz_claw_err_t err = wiz_claw_tls_init(&s_photo_tls,
                                             g_photo_ctx.socket_no,
                                             g_photo_ctx.timeout_ms);
    if (err != WIZ_CLAW_OK) {
        printf("[sendPhoto] TLS init failed (%d)\n", (int)err);
        return err;
    }

    err = wiz_claw_tls_connect(&s_photo_tls, ip, TG_PORT,
                                TG_HOST, g_photo_ctx.timeout_ms);
    if (err != WIZ_CLAW_OK) {
        printf("[sendPhoto] TLS connect failed (%d)\n", (int)err);
        wiz_claw_tls_free(&s_photo_tls);
        return err;
    }

    wiz_claw_tls_write(&s_photo_tls, (const uint8_t *)req, (size_t)rlen);
    wiz_claw_tls_write(&s_photo_tls, (const uint8_t *)prefix, (size_t)plen);
    wiz_claw_tls_write(&s_photo_tls, jpeg, jpeg_size);
    wiz_claw_tls_write(&s_photo_tls, (const uint8_t *)SUFFIX, slen);

    static uint8_t resp[256];
    int32_t n = wiz_claw_tls_read(&s_photo_tls, resp, sizeof(resp) - 1, 15000);
    if (n > 0) {
        resp[n] = '\0';
        printf("[sendPhoto] %.*s\n", (n > 80 ? 80 : (int)n), (char *)resp);
        err = (strstr((char *)resp, " 200 ") != NULL) ? WIZ_CLAW_OK : WIZ_CLAW_ERR_FAIL;
    }

    wiz_claw_tls_close(&s_photo_tls);
    wiz_claw_tls_free(&s_photo_tls);
    return err;
}

/* ── Vision API: JPEG → Groq/Cloudflare multimodal LLM → text ──────
 *
 * JPEG 바이너리를 base64로 인코딩한 후 data URI로 Cloudflare→Groq 전송.
 * 청크 스트리밍 방식으로 추가 힙 없이 110KB+ base64를 직접 write.
 * 반환값: malloc'd 분석 문자열 (호출자가 free), 실패 시 NULL.
 */
#define VISION_PORT  443

#define B64_IN_CHUNK   (3 * 1024)          /* multiple of 3 */
#define B64_OUT_CHUNK  (4 * 1024 + 4)

static char *analyze_image_with_llm(const uint8_t *jpeg, size_t jpeg_size,
                                     const char *question)
{
    /* JSON_PREFIX: model은 런타임 설정에서 읽음 */
    char json_prefix[256];
    int pfx_len = snprintf(json_prefix, sizeof(json_prefix),
        "{\"model\":\"%s\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,",
        g_settings.llm_model);
    if (pfx_len <= 0 || (size_t)pfx_len >= sizeof(json_prefix)) {
        printf("[vision] json_prefix overflow\n");
        return NULL;
    }

    static const char JSON_MID[] =
        "\"}},{\"type\":\"text\",\"text\":\"";
    static const char JSON_SUFFIX[] =
        "\"}]}],\"max_tokens\":256}";

    const char *prompt = (question && question[0]) ? question
                       : "Describe what you see in this image concisely.";

    size_t prefix_len = (size_t)pfx_len;
    size_t mid_len    = sizeof(JSON_MID) - 1;
    size_t prompt_len = strlen(prompt);
    size_t suffix_len = sizeof(JSON_SUFFIX) - 1;
    size_t b64_len    = ((jpeg_size + 2) / 3) * 4;
    size_t body_len   = prefix_len + b64_len + mid_len + prompt_len + suffix_len;

    char req[768];
    int rlen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        g_settings.vision_path,
        g_settings.vision_host,
        g_settings.llm_api_key,
        (unsigned)body_len);
    if (rlen <= 0 || (size_t)rlen >= sizeof(req)) {
        printf("[vision] req header overflow\n");
        return NULL;
    }

    uint8_t ip[4];
    if (wiz_claw_net_dns_resolve(g_settings.vision_host, ip) != WIZ_CLAW_OK) {
        printf("[vision] DNS failed\n");
        return NULL;
    }

    static wiz_claw_tls_t s_vision_tls;
    if (wiz_claw_tls_init(&s_vision_tls, g_vision_ctx.socket_no,
                           g_vision_ctx.timeout_ms) != WIZ_CLAW_OK) {
        printf("[vision] TLS init failed\n");
        return NULL;
    }
    if (wiz_claw_tls_connect(&s_vision_tls, ip, VISION_PORT,
                              g_settings.vision_host,
                              g_vision_ctx.timeout_ms) != WIZ_CLAW_OK) {
        printf("[vision] TLS connect failed\n");
        wiz_claw_tls_free(&s_vision_tls);
        return NULL;
    }

    wiz_claw_tls_write(&s_vision_tls, (const uint8_t *)req, (size_t)rlen);
    wiz_claw_tls_write(&s_vision_tls, (const uint8_t *)json_prefix, prefix_len);

    /* stream base64 in chunks — avoids ~110KB heap allocation */
    static unsigned char s_b64_out[B64_OUT_CHUNK];
    size_t off = 0;
    while (off < jpeg_size) {
        size_t in  = (jpeg_size - off < B64_IN_CHUNK) ? (jpeg_size - off) : B64_IN_CHUNK;
        size_t out = 0;
        mbedtls_base64_encode(s_b64_out, sizeof(s_b64_out), &out, jpeg + off, in);
        wiz_claw_tls_write(&s_vision_tls, s_b64_out, out);
        off += in;
    }

    wiz_claw_tls_write(&s_vision_tls, (const uint8_t *)JSON_MID, mid_len);
    wiz_claw_tls_write(&s_vision_tls, (const uint8_t *)prompt, prompt_len);
    wiz_claw_tls_write(&s_vision_tls, (const uint8_t *)JSON_SUFFIX, suffix_len);

    static uint8_t s_vision_resp[4096];
    int32_t total = 0, n;
    do {
        n = wiz_claw_tls_read(&s_vision_tls, s_vision_resp + total,
                               sizeof(s_vision_resp) - 1 - (size_t)total, 60000);
        if (n > 0) total += n;
    } while (n > 0 && (size_t)total < sizeof(s_vision_resp) - 1);

    wiz_claw_tls_close(&s_vision_tls);
    wiz_claw_tls_free(&s_vision_tls);

    if (total <= 0) {
        printf("[vision] empty response\n");
        return NULL;
    }
    s_vision_resp[total] = '\0';
    printf("[vision] resp %d bytes: %.200s\n", total, (char *)s_vision_resp);

    /* skip HTTP headers */
    char *body = strstr((char *)s_vision_resp, "\r\n\r\n");
    if (!body) { return NULL; }
    body += 4;

    /* decode chunked transfer encoding (Cloudflare always sends chunked) */
    if (strstr((char *)s_vision_resp, "chunked")) {
        char *src = body, *dst = body;
        char *end = (char *)s_vision_resp + total;
        while (src < end) {
            char *crlf = (char *)memchr(src, '\r', (size_t)(end - src));
            if (!crlf || crlf + 1 >= end || crlf[1] != '\n') break;
            size_t csz = (size_t)strtoul(src, NULL, 16);
            if (csz == 0) break;
            src = crlf + 2;
            if (src + csz > end) { csz = (size_t)(end - src); }
            memmove(dst, src, csz);
            dst += csz;
            src += csz;
            if (src + 1 < end && src[0] == '\r' && src[1] == '\n') src += 2;
        }
        *dst = '\0';
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        printf("[vision] JSON parse failed\n");
        return NULL;
    }
    char *result = NULL;
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(
                         cJSON_GetArrayItem(choices, 0), "message");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (cJSON_IsString(content) && content->valuestring) {
            result = strdup(content->valuestring);
        }
    }
    cJSON_Delete(root);
    return result;
}

/* ── capture_photo 툴 핸들러 ─────────────────────────────────────
 *
 * Phase 3: CAPTURE_REQ 전송 → Core1이 CHUNK_DATA 조립 →
 *          CHUNK_END 신호 수신 → Vision API 분석 → Telegram sendPhoto + sendMessage.
 */
static wiz_claw_err_t capture_photo_tool_handler(const char  *name,
                                                  const char  *arguments_json,
                                                  char       **out_result_json,
                                                  void        *user_ctx)
{
    (void)user_ctx; (void)name;

    /* optional "question" arg → passed to Vision API */
    char question[256] = "";
    if (arguments_json) {
        const char *qp = strstr(arguments_json, "\"question\"");
        if (qp) {
            qp = strchr(qp, ':');
            if (qp) {
                qp = strchr(qp, '"');
                if (qp) {
                    qp++;
                    const char *qe = strchr(qp, '"');
                    if (qe) {
                        size_t qlen = (size_t)(qe - qp);
                        if (qlen >= sizeof(question)) qlen = sizeof(question) - 1;
                        memcpy(question, qp, qlen);
                        question[qlen] = '\0';
                    }
                }
            }
        }
    }

    if (!g_photo_buf) {
        g_photo_buf = malloc(PHOTO_BUF_MAX);
        if (!g_photo_buf) {
            *out_result_json = strdup("{\"error\":\"OOM\"}");
            return WIZ_CLAW_OK;
        }
    }

    g_photo_size      = 0;
    g_photo_ready     = false;
    g_photo_capturing = true;
    strncpy(g_photo_mime, "image/jpeg", sizeof(g_photo_mime) - 1);

    wiz_spi_slave_send(SPI_CMD_CAPTURE_REQ, NULL, 0);
    printf("[capture] CAPTURE_REQ sent → waiting...\n");

    /* Core1이 청크를 수신하는 동안 Core0은 90s 대기.
     * ESP32 최초 카메라 오픈(settle ~30s) + JPEG 인코딩 + SPI 전송 포함. */
    absolute_time_t deadline = make_timeout_time_ms(90000);
    while (!g_photo_ready && !time_reached(deadline)) {
        sleep_ms(10);
    }
    g_photo_capturing = false;

    if (!g_photo_ready || g_photo_size == 0) {
        printf("[capture] timeout or empty (%u bytes)\n", (unsigned)g_photo_size);
        *out_result_json = strdup("{\"error\":\"capture timeout\"}");
        return WIZ_CLAW_OK;
    }
    printf("[capture] %u bytes, mime=%s → vision API + Telegram\n",
           (unsigned)g_photo_size, g_photo_mime);

    /* Vision API: base64 JPEG → LLM analysis */
    char *analysis = analyze_image_with_llm(g_photo_buf, g_photo_size,
                                             question[0] ? question : NULL);
    printf("[vision] %s\n", analysis ? analysis : "(no result)");

    wiz_claw_err_t ret = send_photo_to_telegram(g_chat_id, g_photo_buf, g_photo_size);
    if (ret == WIZ_CLAW_OK) {
        if (analysis && analysis[0]) {
            wiz_claw_tg_send_text(&g_tg_send_cfg, g_chat_id, analysis);
        }
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"sent\":true,\"bytes\":%u,\"analyzed\":%s}",
                 (unsigned)g_photo_size, analysis ? "true" : "false");
        *out_result_json = strdup(buf);
    } else {
        *out_result_json = strdup("{\"error\":\"sendPhoto failed\"}");
    }
    free(analysis);
    return WIZ_CLAW_OK;
}

/* ── exec_lua 툴 핸들러 ───────────────────────────────────────────
 *
 * Phase 2: Lua 코드를 SPI_CMD_LUA_EXEC 로 ESP32에 전송.
 * arguments JSON: {"script":"<Lua code>"}
 */
static wiz_claw_err_t exec_lua_tool_handler(const char  *name,
                                             const char  *arguments_json,
                                             char       **out_result_json,
                                             void        *user_ctx)
{
    (void)user_ctx;
    (void)name;

    cJSON *args = cJSON_Parse(arguments_json);
    if (!args) {
        *out_result_json = strdup("{\"error\":\"invalid JSON\"}");
        return WIZ_CLAW_OK;
    }

    const cJSON *script_item = cJSON_GetObjectItemCaseSensitive(args, "script");
    if (!cJSON_IsString(script_item) || !script_item->valuestring) {
        cJSON_Delete(args);
        *out_result_json = strdup("{\"error\":\"missing script field\"}");
        return WIZ_CLAW_OK;
    }

    const char *script    = script_item->valuestring;
    size_t      script_len = strlen(script);

    if (script_len == 0 || script_len > SPI_CLAW_MAX_CHUNK) {
        cJSON_Delete(args);
        *out_result_json = strdup("{\"error\":\"script empty or exceeds 4096 bytes\"}");
        return WIZ_CLAW_OK;
    }

    wiz_spi_slave_send(SPI_CMD_LUA_EXEC,
                       (const uint8_t *)script, (uint16_t)script_len);
    printf("[tool] exec_lua: %u bytes → ESP32\n", (unsigned)script_len);

    cJSON_Delete(args);
    *out_result_json = strdup("{\"submitted\":true}");
    return WIZ_CLAW_OK;
}

/* ── 통합 툴 디스패처 ─────────────────────────────────────────────
 *
 * LLM 툴 이름에 따라 적절한 핸들러로 라우팅.
 */
static wiz_claw_err_t main_tool_handler(const char  *name,
                                         const char  *arguments_json,
                                         char       **out_result_json,
                                         void        *user_ctx)
{
    if (strcmp(name, "set_gpio") == 0) {
        return gpio_tool_handler(name, arguments_json, out_result_json, user_ctx);
    }
    if (strcmp(name, "exec_lua") == 0) {
        return exec_lua_tool_handler(name, arguments_json, out_result_json, user_ctx);
    }
    if (strcmp(name, "capture_photo") == 0) {
        return capture_photo_tool_handler(name, arguments_json, out_result_json, user_ctx);
    }
    *out_result_json = strdup("{\"error\":\"unknown tool\"}");
    return WIZ_CLAW_OK;
}

/* ── tools JSON (OpenAI function-calling 형식) ──────────────────── */
static const char *TOOLS_JSON =
    "["
    "{"
    "  \"type\": \"function\","
    "  \"function\": {"
    "    \"name\": \"set_gpio\","
    "    \"description\": \"Set a GPIO pin on the ESP32 host MCU HIGH or LOW, or toggle it.\","
    "    \"parameters\": {"
    "      \"type\": \"object\","
    "      \"properties\": {"
    "        \"pin\":   {\"type\": \"integer\", \"description\": \"ESP32 GPIO pin number\"},"
    "        \"state\": {\"type\": \"string\",  \"enum\": [\"on\",\"off\",\"toggle\"]}"
    "      },"
    "      \"required\": [\"pin\", \"state\"]"
    "    }"
    "  }"
    "},"
    "{"
    "  \"type\": \"function\","
    "  \"function\": {"
    "    \"name\": \"exec_lua\","
    "    \"description\": \"Execute a Lua script on the ESP32 host MCU asynchronously.\","
    "    \"parameters\": {"
    "      \"type\": \"object\","
    "      \"properties\": {"
    "        \"script\": {\"type\": \"string\","
    "                     \"description\": \"Lua source code to execute (max 4096 bytes)\"}"
    "      },"
    "      \"required\": [\"script\"]"
    "    }"
    "  }"
    "}"
    ","
    "{"
    "  \"type\": \"function\","
    "  \"function\": {"
    "    \"name\": \"capture_photo\","
    "    \"description\": \"Capture a photo on the ESP32, analyze it with a vision LLM, "
                           "and send the photo and analysis text to Telegram.\","
    "    \"parameters\": {"
    "      \"type\": \"object\","
    "      \"properties\": {},"
    "      \"required\": []"
    "    }"
    "  }"
    "}"
    "]";

/* ── 시스템 프롬프트 ─────────────────────────────────────────────── */
static const char *SYSTEM_PROMPT =
    "You are a helpful assistant running on an RP2350 microcontroller "
    "acting as a network coprocessor for an ESP32-S3 host MCU. "
    "You can control ESP32 GPIO pins with set_gpio, "
    "run Lua scripts on the ESP32 with exec_lua, "
    "and capture and visually analyze a photo with capture_photo. "
    "capture_photo sends the image to Telegram and calls a vision LLM to analyze it; "
    "use the optional 'question' argument to ask something specific about the scene. "
    "After calling a tool and receiving its result, always reply with a short "
    "text message confirming what was done — do NOT call the same tool again. "
    "Respond concisely in the same language the user writes in.";

/* ── HTTP 프록시 응답 전송 (Core0) ──────────────────────────────────
 * wiz_claw_http_post_cb 결과를 SPI_CMD_HTTP_RESP + RESP_BODY chunks +
 * SPI_CMD_HTTP_RESP_END 로 ESP32에 돌려준다.
 */
/* http_status: actual Groq HTTP status (200/400/429/…), or 0 for connection error */
static void _proxy_send_response(int http_status, const char *body)
{
    int    status   = (http_status > 0) ? http_status : 500;
    size_t body_len = body ? strlen(body) : 0;

    char meta[64];
    int  mlen = snprintf(meta, sizeof(meta),
                         "{\"status\":%d,\"body_len\":%zu}", status, body_len);
    wiz_spi_slave_send(SPI_CMD_HTTP_RESP, (const uint8_t *)meta, (uint16_t)mlen);

    if (body && body_len > 0) {
        const char *ptr = body;
        size_t remaining = body_len;
        while (remaining > 0) {
            uint16_t n = (uint16_t)(remaining > SPI_CLAW_MAX_CHUNK
                                     ? SPI_CLAW_MAX_CHUNK : remaining);
            wiz_spi_slave_send(SPI_CMD_HTTP_RESP_BODY, (const uint8_t *)ptr, n);
            ptr       += n;
            remaining -= n;
            sleep_ms(5);
        }
    }

    wiz_spi_slave_send(SPI_CMD_HTTP_RESP_END, NULL, 0);
    printf("[proxy] response sent status=%d body=%zu bytes\n", status, body_len);
}

/* 부팅 드레인용 noop 콜백 — 메시지 수신만 ACK, 처리 안 함 */
static void tg_drain_noop(const wiz_claw_tg_message_t *msg, void *ctx)
{
    (void)msg; (void)ctx;
}

/* ── 메시지 콜백 컨텍스트 ────────────────────────────────────────── */
typedef struct {
    const wiz_claw_tg_config_t    *tg_send;
    const wiz_claw_agent_config_t *agent;
    cJSON                         *session;
} agent_ctx_t;

/* ── Telegram 메시지 수신 콜백 ────────────────────────────────────── */
static void on_telegram_message(const wiz_claw_tg_message_t *msg,
                                 void                        *user_ctx)
{
    agent_ctx_t *ctx = (agent_ctx_t *)user_ctx;
    if (!msg->text || msg->text[0] == '\0') { return; }
    printf("[Telegram] from=%s: %s\n", msg->sender_id, msg->text);

    strncpy(g_chat_id, msg->chat_id, sizeof(g_chat_id) - 1);
    g_chat_id[sizeof(g_chat_id) - 1] = '\0';

    const char *send_text = NULL;
    char *reply   = NULL;
    char *err_msg = NULL;
    bool used_relay = false;

    /* ── ESP32 CLAW 릴레이 시도 ─────────────────────────────────── */
    if (g_esp32_alive) {
        /* session_id = "tg_<chat_id>" → CLAW 메모리 per-user 유지 */
        char session_id[80];
        snprintf(session_id, sizeof(session_id), "tg_%s", msg->chat_id);

        cJSON *req = cJSON_CreateObject();
        if (req) {
            cJSON_AddStringToObject(req, "session_id", session_id);
            cJSON_AddStringToObject(req, "chat_id",    msg->chat_id);
            cJSON_AddStringToObject(req, "sender",     msg->sender_id ? msg->sender_id : "unknown");
            cJSON_AddStringToObject(req, "text",       msg->text);
            char *req_str = cJSON_PrintUnformatted(req);
            cJSON_Delete(req);

            if (req_str) {
                /* 응답 플래그 초기화 */
                uint32_t save = spin_lock_blocking(g_llm_spin);
                g_llm_resp_ready = false;
                g_llm_resp_ok    = false;
                spin_unlock(g_llm_spin, save);

                wiz_spi_slave_send(SPI_CMD_LLM_REQ,
                                   (const uint8_t *)req_str,
                                   (uint16_t)strlen(req_str));
                free(req_str);
                printf("[relay] LLM_REQ sent to ESP32, waiting up to %ums\n",
                       LLM_RELAY_TIMEOUT_MS);

                /* 응답 대기 (Core1이 SPI poll 계속 수행)
                 * 루프 내부에서 HTTP proxy 요청도 처리한다:
                 * ESP32가 WiFi 없이 Groq 호출이 필요할 때 SPI로 proxy 요청을
                 * 보내면 Core0이 여기서 받아 Ethernet으로 HTTP POST를 대신 수행.
                 */
                uint32_t deadline = to_ms_since_boot(get_absolute_time()) + LLM_RELAY_TIMEOUT_MS;
                bool got = false;
                while (to_ms_since_boot(get_absolute_time()) < deadline) {
                    save = spin_lock_blocking(g_llm_spin);
                    got = g_llm_resp_ready;
                    spin_unlock(g_llm_spin, save);
                    if (got) break;

                    /* HTTP proxy: ESP32 body 수신 완료 → HTTP POST → 응답 전송 */
                    if (g_proxy_ready && g_proxy_body) {
                        printf("[proxy] forwarding %u bytes to %s\n",
                               (unsigned)g_proxy_body_rcv, g_proxy_url);
                        char *resp = NULL;
                        wiz_claw_err_t perr = wiz_claw_http_post_cb(
                            g_proxy_url, g_proxy_auth,
                            (char *)g_proxy_body, &resp, &g_llm_ctx);
                        int http_status = (perr == WIZ_CLAW_OK)
                                          ? wiz_claw_http_last_status() : 0;
                        _proxy_send_response(http_status, resp);
                        free(resp);
                        free(g_proxy_body); g_proxy_body = NULL;
                        g_proxy_body_len = 0;
                        g_proxy_body_rcv = 0;
                        g_proxy_ready    = false;
                    }

                    sleep_ms(10);
                }

                if (got && g_llm_resp_ok) {
                    send_text  = g_llm_resp_text;
                    used_relay = true;
                    g_stat_relay_ok++;
                    g_relay_to_streak = 0;   /* agent answered → alive */
                    printf("[relay] ESP32 CLAW response received\n");
                } else if (got) {
                    g_stat_relay_err++;
                    g_relay_to_streak = 0;   /* got a response (err) → agent alive */
                    printf("[relay] ESP32 returned error — immediate local Groq fallback\n");
                    /* g_llm_resp_ok=false: ESP32 context/LLM 실패, 로컬 fallback 시도 */
                } else {
                    g_stat_relay_timeout++;
                    g_relay_to_streak++;
                    printf("[relay] timeout — falling back to local Groq (streak=%lu)\n",
                           (unsigned long)g_relay_to_streak);
                    /* g_esp32_alive 유지: ESP32 STATUS 패킷 오면 자동 복구 */
                    if (g_esp32_alive && g_relay_to_streak >= RELAY_HUNG_STREAK) {
                        /* STATUS still arriving but agent not answering →
                         * agent logically hung (not crashed). This is the case a
                         * dumb watchdog IC cannot see. Guardian would HW-reset the
                         * ESP here via the EN line — NEXT step, not wired yet. */
                        printf("[guardian] ESP32 agent HUNG (streak=%lu, STATUS alive) "
                               "— HW reset pending (EN line TODO)\n",
                               (unsigned long)g_relay_to_streak);
                    }
                }
            }
        }
    }

    /* ── 로컬 Groq fallback ─────────────────────────────────────── */
    if (!used_relay) {
        if (g_settings.llm_api_key[0] == '\0') {
            /* No local LLM key configured: a call here always 401s (see
             * DEVLOG.md S-track 15th entry) — wastes a TLS round trip and
             * returns a misleading "처리 중 오류" instead of the real cause.
             * Skip the doomed call and say so honestly. Configure a key via
             * the web dashboard (http://<pico-ip>/) to enable this fallback. */
            g_stat_local_fb_unavail++;
            printf("[agent] local fallback unavailable: no llm_api_key configured\n");
            send_text = "일시적으로 ESP32 연결이 끊어졌고, 로컬 백업 응답도 설정되지 않았습니다. "
                        "잠시 후 다시 시도해주세요.";
        } else {
            g_stat_local_fb++;
            if (ctx->session == NULL) {
                ctx->session = wiz_claw_session_create();
            }
            wiz_claw_err_t ret = wiz_claw_agent_run(ctx->agent, ctx->session,
                                                     msg->text, &reply, &err_msg);
            if (ret == WIZ_CLAW_OK && reply) {
                send_text = reply;
            } else {
                printf("[agent] error: %s\n", err_msg ? err_msg : "unknown");
                send_text = "죄송합니다, 처리 중 오류가 발생했습니다.";
                cJSON_Delete(ctx->session);
                ctx->session = NULL;
            }
        }
    }

    wiz_claw_tg_send_text(ctx->tg_send, msg->chat_id, send_text);
    printf("[reply] %s\n", send_text);

    free(reply);
    free(err_msg);
}

/* ── Core 1: SPI slave 전용 루프 ────────────────────────────────────
 * Core 0이 Telegram TLS로 블로킹되는 동안 Core 1이 SPI를 처리.
 */
static void core1_spi_entry(void)
{
    /* flash 쓰기 시 Core 0의 multicore_lockout_start_blocking()에 응답 */
    multicore_lockout_victim_init();
    while (1) {
        wiz_spi_slave_poll();
        tight_loop_contents();
    }
}

/* ── SPI CRC 에러 콜백 ──────────────────────────────────────────────
 * HTTP_BODY/BODY_END CRC 에러 시 proxy body 버퍼를 해제하고 플래그 설정.
 * 응답 전송은 BODY_END 도착 후 Core0에서 처리 — BODY TX 중 응답 전송 시
 * ESP32 s_rx_task와 spi_wiz_send(chunk) 간에 SPI bus race 발생하므로.
 */
static void on_spi_crc_error(uint8_t raw_cmd, void *user_ctx)
{
    (void)user_ctx;
    if (raw_cmd == SPI_CMD_HTTP_BODY || raw_cmd == SPI_CMD_HTTP_BODY_END ||
        raw_cmd == SPI_CMD_HTTP_REQ) {
        if (g_proxy_body || g_proxy_body_len > 0) {
            printf("[proxy] CRC error on HTTP stream cmd=0x%02X — will respond 500 on BODY_END\n",
                   raw_cmd);
            free(g_proxy_body);
            g_proxy_body     = NULL;
            g_proxy_body_len = 0;
            g_proxy_body_rcv = 0;
            g_proxy_ready    = false;
            g_proxy_crc_err  = true;
        }
    }
}

/* ── SPI 수신 콜백 ───────────────────────────────────────────────────
 * CMD_PING은 wiz_spi_slave.c 내부에서 PONG으로 자동 응답.
 * CMD_ACK: ESP32가 GPIO_SET / LUA_EXEC 처리 완료 후 전송.
 */
static void on_spi_rx(spi_claw_cmd_t cmd, uint8_t seq, const uint8_t *payload,
                      uint16_t len, void *user_ctx)
{
    (void)user_ctx;
    switch (cmd) {
    case SPI_CMD_PING:
        if (!g_esp32_alive) {
            printf("[spi_rx] ESP32 PING — CLAW relay pre-enabled\n");
        }
        g_esp32_alive = true;
        g_esp32_last_seen_ms = to_ms_since_boot(get_absolute_time());
        break;

    case SPI_CMD_ESP_STATUS: {
        if (!g_esp32_alive) {
            printf("[spi_rx] ESP32 alive — CLAW relay enabled\n");
        }
        g_esp32_alive = true;
        g_esp32_last_seen_ms = to_ms_since_boot(get_absolute_time());
        /* Parse objective health. Payload is small & trusted but NOT
         * NUL-terminated, so copy to a local buffer first. */
        if (payload && len > 0 && len < 192) {
            char buf[192];
            memcpy(buf, payload, len);
            buf[len] = '\0';
            uint32_t seq_v = 0, up_v = 0, heap_v = 0;
            bool have_seq = esp_js_uint(buf, "seq", &seq_v);
            esp_js_uint(buf, "up", &up_v);
            esp_js_uint(buf, "heap", &heap_v);
            /* Reboot: seq reset to a lower value, or uptime regressed. */
            if (g_esp_valid && have_seq &&
                (seq_v < g_esp_seq || (up_v + 5u) < g_esp_up_s)) {
                printf("[guardian] ESP32 reboot detected (seq %lu->%lu, up %lus->%lus)\n",
                       (unsigned long)g_esp_seq, (unsigned long)seq_v,
                       (unsigned long)g_esp_up_s, (unsigned long)up_v);
                g_relay_to_streak = 0;   /* fresh ESP */
            }
            g_esp_seq   = seq_v;
            g_esp_up_s  = up_v;
            g_esp_heap  = heap_v;
            g_esp_wifi  = esp_js_bool_true(buf, "wifi");
            g_esp_proxy = esp_js_bool_true(buf, "proxy");
            g_esp_valid = true;
        }
        break;
    }

    case SPI_CMD_ACK:
        if (payload && len > 0) {
            printf("[spi_rx] ACK seq=%u payload=%.*s\n",
                   seq, (int)len, (const char *)payload);
        } else {
            printf("[spi_rx] ACK seq=%u\n", seq);
        }
        break;

    case SPI_CMD_LLM_RESP: {
        if (!payload || len == 0) break;
        char *js = malloc(len + 1);
        if (!js) break;
        memcpy(js, payload, len);
        js[len] = '\0';
        cJSON *root = cJSON_Parse(js);
        free(js);
        if (!root) { printf("[spi_rx] LLM_RESP: parse failed\n"); break; }
        const char *session = cJSON_GetStringValue(cJSON_GetObjectItem(root, "session_id"));
        const char *text    = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        bool ok = cJSON_IsTrue(cJSON_GetObjectItem(root, "ok"));
        {
            uint32_t save = spin_lock_blocking(g_llm_spin);
            if (ok && text) {
                strncpy(g_llm_resp_text, text, LLM_RESP_TEXT_MAX);
                g_llm_resp_text[LLM_RESP_TEXT_MAX] = '\0';
                if (session) {
                    strncpy(g_llm_resp_session, session, sizeof(g_llm_resp_session) - 1);
                    g_llm_resp_session[sizeof(g_llm_resp_session) - 1] = '\0';
                }
                g_llm_resp_ok    = true;
                g_llm_resp_ready = true;
                spin_unlock(g_llm_spin, save);
                printf("[spi_rx] LLM_RESP ok session=%s len=%u\n",
                       session ? session : "?", (unsigned)strlen(text));
            } else {
                /* ok=false: wake relay loop immediately so it falls back without waiting 90s */
                g_llm_resp_ok    = false;
                g_llm_resp_ready = true;
                spin_unlock(g_llm_spin, save);
                printf("[spi_rx] LLM_RESP error — waking relay for immediate fallback\n");
            }
        }
        cJSON_Delete(root);
        break;
    }

    case SPI_CMD_CHUNK_DATA:
        if (g_photo_capturing && g_photo_buf) {
            if (g_photo_size + len <= PHOTO_BUF_MAX) {
                memcpy(g_photo_buf + g_photo_size, payload, len);
                g_photo_size += len;
            } else {
                printf("[spi_rx] CHUNK_DATA overflow: %u+%u > %u\n",
                       (unsigned)g_photo_size, len, PHOTO_BUF_MAX);
            }
        } else {
            printf("[spi_rx] CHUNK_DATA seq=%u len=%u (ignored)\n", seq, len);
        }
        break;

    case SPI_CMD_EVENT:
        if (payload && len >= 3) {
            /* look for "pir" in payload e.g. {"type":"pir"} */
            const char *ep = (const char *)payload;
            for (uint16_t i = 0; i + 2 < len; i++) {
                if (ep[i] == 'p' && ep[i+1] == 'i' && ep[i+2] == 'r') {
                    printf("[spi_rx] PIR event → auto capture queued\n");
                    g_pir_triggered = true;
                    break;
                }
            }
        }
        break;

    case SPI_CMD_CHUNK_END:
        if (payload && len > 0) {
            /* {"mime":"image/jpeg","size":N} から mime を抽出 */
            const char *mp = strstr((const char *)payload, "\"mime\":\"");
            if (mp) {
                mp += 8;
                const char *ep = strchr(mp, '"');
                if (ep && (size_t)(ep - mp) < sizeof(g_photo_mime)) {
                    memcpy(g_photo_mime, mp, (size_t)(ep - mp));
                    g_photo_mime[(size_t)(ep - mp)] = '\0';
                }
            }
        }
        printf("[spi_rx] CHUNK_END seq=%u mime=%s total=%u bytes\n",
               seq, g_photo_mime, (unsigned)g_photo_size);
        g_photo_ready = true;
        break;

    case SPI_CMD_HTTP_REQ: {
        /* {"url":"...","auth":"Bearer key","body_len":N} */
        if (!payload || len == 0 || len >= 768) { break; }
        char js[768]; memcpy(js, payload, len); js[len] = '\0';
        cJSON *r = cJSON_Parse(js);
        if (!r) { printf("[proxy] HTTP_REQ parse failed\n"); break; }
        const char *url  = cJSON_GetStringValue(cJSON_GetObjectItem(r, "url"));
        const char *auth = cJSON_GetStringValue(cJSON_GetObjectItem(r, "auth"));
        cJSON *bl = cJSON_GetObjectItem(r, "body_len");
        uint32_t blen = cJSON_IsNumber(bl) ? (uint32_t)bl->valuedouble : 0;

        strncpy(g_proxy_url,  url  ? url  : "", HTTP_PROXY_URL_MAX  - 1);
        strncpy(g_proxy_auth, auth ? auth : "", HTTP_PROXY_AUTH_MAX - 1);
        free(g_proxy_body);
        g_proxy_body = (blen > 0 && blen <= HTTP_PROXY_BODY_MAX)
                       ? malloc(blen + 1) : NULL;
        g_proxy_body_len = blen;
        g_proxy_body_rcv = 0;
        g_proxy_ready    = false;
        g_proxy_crc_err  = false;
        cJSON_Delete(r);
        printf("[proxy] HTTP_REQ url=%.60s body_len=%u\n",
               g_proxy_url, (unsigned)blen);
        break;
    }

    case SPI_CMD_HTTP_BODY: {
        if (!g_proxy_body || !payload || len == 0) { break; }
        if (g_proxy_body_rcv + len <= g_proxy_body_len) {
            memcpy(g_proxy_body + g_proxy_body_rcv, payload, len);
            g_proxy_body_rcv += len;
        } else {
            printf("[proxy] BODY overflow %u+%u > %u\n",
                   (unsigned)g_proxy_body_rcv, len, (unsigned)g_proxy_body_len);
        }
        break;
    }

    case SPI_CMD_HTTP_BODY_END: {
        if (g_proxy_crc_err) {
            /* CRC error occurred during body receive — respond with 500 now that
             * ESP32 has finished sending (no more SPI TX race for s_rx_task). */
            printf("[proxy] BODY_END after CRC error, sending 500\n");
            _proxy_send_response(0, NULL);
            g_proxy_crc_err  = false;
            free(g_proxy_body);
            g_proxy_body     = NULL;
            g_proxy_body_len = 0;
            g_proxy_body_rcv = 0;
            g_proxy_ready    = false;
        } else if (g_proxy_body) {
            if (g_proxy_body_rcv < g_proxy_body_len) {
                printf("[proxy] BODY incomplete %u/%u, sending error\n",
                       (unsigned)g_proxy_body_rcv, (unsigned)g_proxy_body_len);
                _proxy_send_response(0, NULL);
                free(g_proxy_body);
                g_proxy_body     = NULL;
                g_proxy_body_len = 0;
                g_proxy_body_rcv = 0;
                g_proxy_ready    = false;
            } else {
                g_proxy_body[g_proxy_body_rcv] = '\0';
                g_proxy_ready = true;
                printf("[proxy] BODY_END rcv=%u/%u\n",
                       (unsigned)g_proxy_body_rcv, (unsigned)g_proxy_body_len);
            }
        }
        break;
    }

    default:
        printf("[spi_rx] unhandled cmd=0x%02X seq=%u len=%u\n", cmd, seq, len);
        break;
    }
}

/* ── main ───────────────────────────────────────────────────────── */

int main(void)
{
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== wiz_claw_spi_host starting (Phase 2) ===\n");

    g_llm_spin = spin_lock_instance(spin_lock_claim_unused(true));

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    /* flash에서 설정 로드 (magic 불일치 시 기본값) */
    wiz_claw_settings_load(&g_settings);

    /* g_settings으로 네트워크 정보 구성 */
    memset(&g_net_info, 0, sizeof(g_net_info));
    g_net_info.mac[0] = 0x00; g_net_info.mac[1] = 0x08;
    g_net_info.mac[2] = 0xDC; g_net_info.mac[3] = 0xAB;
    g_net_info.mac[4] = 0xCD; g_net_info.mac[5] = 0xEF;
    memcpy(g_net_info.ip,  g_settings.ip,  4);
    memcpy(g_net_info.sn,  g_settings.sn,  4);
    memcpy(g_net_info.gw,  g_settings.gw,  4);
    memcpy(g_net_info.dns, g_settings.dns, 4);
    g_net_info.dhcp = NETINFO_STATIC;

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    wiz_claw_net_config_t net_cfg = {
        .dns_server_ip = {
            g_settings.dns[0], g_settings.dns[1],
            g_settings.dns[2], g_settings.dns[3]
        },
        .dns_socket    = 5,
        .dhcp_socket   = 0,
    };
    wiz_claw_net_init(&net_cfg);

    add_repeating_timer_ms(-1000, timer_1s_cb, NULL, &g_1s_timer);

    network_initialize(g_net_info);
    wiz_claw_net_print_info(&g_net_info);

    wiz_spi_slave_init(on_spi_rx, NULL);
    wiz_spi_slave_set_crc_err_cb(on_spi_crc_error, NULL);
    printf("[main] SPI slave ready\n");

    wiz_claw_tg_config_t tg_poll_cfg = {
        .bot_token     = g_settings.tg_bot_token,
        .api_base      = "https://api.telegram.org",
        .http_call     = wiz_claw_http_call_cb,
        .http_user_ctx = &g_tg_poll_ctx,
    };
    g_tg_send_cfg = (wiz_claw_tg_config_t){
        .bot_token     = g_settings.tg_bot_token,
        .api_base      = "https://api.telegram.org",
        .http_call     = wiz_claw_http_call_cb,
        .http_user_ctx = &g_tg_send_ctx,
    };

    wiz_claw_llm_config_t llm_cfg = {
        .base_url       = g_settings.llm_base_url,
        .api_key        = g_settings.llm_api_key,
        .model          = g_settings.llm_model,
        .chat_path      = g_settings.llm_chat_path,
        .max_tokens     = (uint32_t)atoi(g_settings.llm_max_tokens),
        .supports_tools = true,
        .http_post      = wiz_claw_http_post_cb,
        .http_user_ctx  = &g_llm_ctx,
    };

    wiz_claw_agent_config_t agent_cfg = {
        .llm                 = llm_cfg,
        .system_prompt       = SYSTEM_PROMPT,
        .tools_json          = TOOLS_JSON,
        .tool_handler        = main_tool_handler,
        .tool_handler_ctx    = NULL,
        .max_tool_iterations = AGENT_MAX_ITER,
    };

    agent_ctx_t agent_ctx = {
        .tg_send = &g_tg_send_cfg,
        .agent   = &agent_cfg,
        .session = NULL,
    };

    multicore_launch_core1(core1_spi_entry);
    printf("[main] Core 1 started for SPI slave polling\n");

    wiz_claw_webserver_init(&g_settings);

    printf("[main] Phase 2 ready — Core 0: Telegram+WebServer / Core 1: SPI slave\n");

    /* ── 부팅 시 미처리 메시지 드레인 ──────────────────────────────
     * timeout=0 short-poll + noop 콜백으로 오래된 메시지 ACK 처리.
     * offset이 더 이상 바뀌지 않으면 큐 비워진 것 → 진짜 루프 시작.
     */
    int64_t offset = 0;
    {
        int64_t prev;
        printf("[main] draining stale Telegram messages...\n");
        do {
            prev = offset;
            wiz_claw_tg_poll(&tg_poll_cfg, 0, &offset,
                             tg_drain_noop, NULL);
        } while (offset != prev);
        printf("[main] drain done, starting from offset=%" PRId64 "\n", offset);
    }

    absolute_time_t next_health = make_timeout_time_ms(30000);
    print_health_line();  /* baseline at boot */

    while (1) {
        wiz_claw_tg_poll(&tg_poll_cfg,
                          TG_POLL_TIMEOUT_SEC,
                          &offset,
                          on_telegram_message,
                          &agent_ctx);

        wiz_claw_webserver_poll();

        check_esp32_alive_timeout();

        if (time_reached(next_health)) {
            print_health_line();
            next_health = make_timeout_time_ms(30000);
        }

        /* PIR auto-capture (set by Core1 SPI callback) */
        if (g_pir_triggered) {
            if (g_chat_id[0] != '\0' && time_reached(g_pir_cooldown_until)) {
                g_pir_triggered = false;
                g_pir_cooldown_until = make_timeout_time_ms(60000); /* 60s cooldown */
                printf("[PIR] capture + TFLite filter + vision\n");
                char *result = NULL;
                capture_photo_tool_handler("capture_photo", "{}", &result, NULL);
                if (result && strstr(result, "\"sent\":true")) {
                    printf("[PIR] alert sent (person detected)\n");
                } else {
                    printf("[PIR] no person or capture failed — no alert\n");
                }
                free(result);
            } else if (g_chat_id[0] == '\0') {
                printf("[PIR] triggered but no chat_id — send a message first\n");
                g_pir_triggered = false;
            }
            /* else: cooldown active, keep flag set, retry next poll cycle */
        }
    }
}
