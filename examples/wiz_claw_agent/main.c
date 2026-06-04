/**
 * wiz_claw_agent — RP2350 + WIZnet Chip Telegram AI Agent 예제
 *
 * 동작:
 *   1. WIZnet Chip 이더넷 초기화 → DHCP IP 획득
 *   2. Telegram getUpdates 폴링 (콜백 방식)
 *   3. 수신 메시지를 OpenAI LLM 에이전트로 전달
 *   4. 에이전트가 tool_call 발행 시 GPIO 제어 (LED on/off/toggle)
 *   5. 최종 답변을 Telegram으로 전송
 *
 * 설정:
 *   아래 #define 세 줄만 채우면 됩니다.
 */

/* ── 사용자 설정 ───────────────────────────────────────────────── */
#define BOT_TOKEN       "xxxxx"
#define OPENAI_API_KEY  "xxxx"
#define AGENT_MODEL     "gpt-4o-mini"
/* ─────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"

#include "wiz_claw_net.h"
#include "wiz_claw_http.h"
#include "wiz_claw_telegram.h"
#include "wiz_claw_llm.h"
#include "wiz_claw_agent.h"

/* ── 상수 ──────────────────────────────────────────────────────── */
#define LED_PIN             PICO_DEFAULT_LED_PIN
#define TG_POLL_TIMEOUT_SEC 20
#define AGENT_MAX_ITER      8

/* ── 소켓 할당 ─────────────────────────────────────────────────────
 *   0 : Telegram getUpdates (long-poll)
 *   1 : Telegram sendMessage
 *   2 : LLM API
 *   5 : DNS (UDP)
 *   6 : DHCP (UDP)
 */
static wiz_claw_http_ctx_t g_tg_poll_ctx = { .socket_no = 0, .timeout_ms = 60000 };
static wiz_claw_http_ctx_t g_tg_send_ctx = { .socket_no = 1, .timeout_ms = 30000 };
static wiz_claw_http_ctx_t g_llm_ctx     = { .socket_no = 2, .timeout_ms = 60000 };

/* ── 네트워크 설정 ─────────────────────────────────────────────── */
static wiz_NetInfo g_net_info = {
    .mac  = { 0x00, 0x08, 0xDC, 0xAB, 0xCD, 0xEF },
    .ip   = { 192, 168, 11, 20 },
    .sn   = { 255, 255, 255, 0 },
    .gw   = { 192, 168, 11, 1 },
    .dns  = { 8, 8, 8, 8 },
    .dhcp = NETINFO_STATIC,
};

/* ── 1초 타이머 ─────────────────────────────────────────────────── */
static struct repeating_timer g_1s_timer;

static bool timer_1s_cb(struct repeating_timer *t)
{
    (void)t;
    wiz_claw_net_1s_tick();
    return true;
}

/* ── GPIO tool 핸들러 ─────────────────────────────────────────────
 *
 * LLM이 호출하는 "set_gpio" 툴.
 * arguments JSON 예: {"pin": 25, "state": "on"}
 */
static wiz_claw_err_t gpio_tool_handler(const char  *name,
                                         const char  *arguments_json,
                                         char       **out_result_json,
                                         void        *user_ctx)
{
    (void)user_ctx;

    if (strcmp(name, "set_gpio") != 0) {
        *out_result_json = strdup("{\"error\":\"unknown tool\"}");
        return WIZ_CLAW_OK;
    }

    int pin = LED_PIN;
    const char *state = "toggle";

    const char *p_pin = strstr(arguments_json, "\"pin\"");
    if (p_pin) {
        p_pin = strchr(p_pin, ':');
        if (p_pin) { pin = atoi(p_pin + 1); }
    }

    const char *p_state = strstr(arguments_json, "\"state\"");
    if (p_state) {
        if (strstr(p_state, "\"on\""))      { state = "on"; }
        else if (strstr(p_state, "\"off\"")) { state = "off"; }
        else                                 { state = "toggle"; }
    }

    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);

    bool current = gpio_get(pin);
    bool next;

    if (strcmp(state, "on") == 0)       { next = true; }
    else if (strcmp(state, "off") == 0) { next = false; }
    else                                { next = !current; }

    gpio_put(pin, next);
    printf("[tool] GPIO %d → %s\n", pin, next ? "ON" : "OFF");

    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"pin\":%d,\"state\":\"%s\"}", pin, next ? "on" : "off");
    *out_result_json = strdup(buf);
    return WIZ_CLAW_OK;
}

/* ── tools JSON (OpenAI function-calling 형식) ──────────────────── */
static const char *TOOLS_JSON =
    "[{"
    "  \"type\": \"function\","
    "  \"function\": {"
    "    \"name\": \"set_gpio\","
    "    \"description\": \"Set a GPIO pin HIGH or LOW, or toggle it. "
                           "Use pin 25 for the onboard LED.\","
    "    \"parameters\": {"
    "      \"type\": \"object\","
    "      \"properties\": {"
    "        \"pin\":   {\"type\": \"integer\", \"description\": \"GPIO pin number\"},"
    "        \"state\": {\"type\": \"string\",  \"enum\": [\"on\",\"off\",\"toggle\"]}"
    "      },"
    "      \"required\": [\"pin\", \"state\"]"
    "    }"
    "  }"
    "}]";

/* ── 시스템 프롬프트 ─────────────────────────────────────────────── */
static const char *SYSTEM_PROMPT =
    "You are a helpful assistant running on an RP2350 microcontroller "
    "connected to a WIZnet WIZnet Ethernet chip. "
    "You can control GPIO pins using the set_gpio tool. "
    "The onboard LED is on GPIO 25. "
    "Respond concisely in the same language the user writes in.";

/* ── 메시지 콜백 컨텍스트 ────────────────────────────────────────── */
typedef struct {
    const wiz_claw_tg_config_t    *tg_send;  /* sendMessage용 설정 (소켓 1) */
    const wiz_claw_agent_config_t *agent;
    cJSON                         *session;  /* 대화 히스토리 */
} agent_ctx_t;

/* ── Telegram 메시지 수신 콜백 ──────────────────────────────────────
 *
 * wiz_claw_tg_poll이 메시지를 받을 때마다 호출됨.
 * 에이전트 실행 → LLM 응답 → Telegram 전송.
 */
static void on_telegram_message(const wiz_claw_tg_message_t *msg,
                                 void                        *user_ctx)
{
    agent_ctx_t *ctx = (agent_ctx_t *)user_ctx;

    if (!msg->text || msg->text[0] == '\0') { return; }

    printf("[Telegram] from=%s: %s\n", msg->sender_id, msg->text);

    if (ctx->session == NULL) {
        ctx->session = wiz_claw_session_create();
    }

    char *reply   = NULL;
    char *err_msg = NULL;

    wiz_claw_err_t ret = wiz_claw_agent_run(ctx->agent, ctx->session,
                                              msg->text, &reply, &err_msg);

    const char *send_text;
    if (ret == WIZ_CLAW_OK && reply) {
        send_text = reply;
    } else {
        printf("[agent] error: %s\n", err_msg ? err_msg : "unknown");
        send_text = "죄송합니다, 처리 중 오류가 발생했습니다.";
        cJSON_Delete(ctx->session);
        ctx->session = NULL;
    }

    wiz_claw_tg_send_text(ctx->tg_send, msg->chat_id, send_text);
    printf("[reply] %s\n", send_text);

    free(reply);
    free(err_msg);
}

/* ── main ───────────────────────────────────────────────────────── */

int main(void)
{
    /* 1. 시리얼 초기화 (USB CDC) */
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== wiz_claw_agent starting ===\n");

    /* 2. LED 초기화 */
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    /* 3. WIZnet Chip 하드웨어 초기화 */
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    /* 4. DNS 모듈 초기화 (호스트명 해석용) */
    wiz_claw_net_config_t net_cfg = {
        .dns_server_ip = { 8, 8, 8, 8 },
        .dns_socket    = 5,
        .dhcp_socket   = 0,
    };
    wiz_claw_net_init(&net_cfg);

    /* 5. 1초 타이머 시작 (DNS_time_handler 틱) */
    add_repeating_timer_ms(-1000, timer_1s_cb, NULL, &g_1s_timer);

    /* 6. 정적 IP 설정 */
    network_initialize(g_net_info);
    wiz_claw_net_print_info(&g_net_info);

    /* 7. Telegram 설정 — poll용(소켓 0)과 send용(소켓 1) 분리 */
    wiz_claw_tg_config_t tg_poll_cfg = {
        .bot_token     = BOT_TOKEN,
        .api_base      = "https://api.telegram.org",
        .http_call     = wiz_claw_http_call_cb,
        .http_user_ctx = &g_tg_poll_ctx,
    };
    wiz_claw_tg_config_t tg_send_cfg = {
        .bot_token     = BOT_TOKEN,
        .api_base      = "https://api.telegram.org",
        .http_call     = wiz_claw_http_call_cb,
        .http_user_ctx = &g_tg_send_ctx,
    };

    /* 8. LLM 설정 */
    wiz_claw_llm_config_t llm_cfg = {
        .base_url       = "https://api.openai.com",
        .api_key        = OPENAI_API_KEY,
        .model          = AGENT_MODEL,
        .chat_path      = "/v1/chat/completions",
        .max_tokens     = 512,
        .supports_tools = true,
        .http_post      = wiz_claw_http_post_cb,
        .http_user_ctx  = &g_llm_ctx,
    };

    /* 9. 에이전트 설정 */
    wiz_claw_agent_config_t agent_cfg = {
        .llm                 = llm_cfg,
        .system_prompt       = SYSTEM_PROMPT,
        .tools_json          = TOOLS_JSON,
        .tool_handler        = gpio_tool_handler,
        .tool_handler_ctx    = NULL,
        .max_tool_iterations = AGENT_MAX_ITER,
    };

    /* 10. 콜백 컨텍스트 초기화 */
    agent_ctx_t agent_ctx = {
        .tg_send = &tg_send_cfg,
        .agent   = &agent_cfg,
        .session = NULL,
    };

    printf("Telegram 폴링 시작...\n");

    /* 11. 메인 루프 — wiz_claw_tg_poll이 메시지마다 on_telegram_message 호출 */
    int64_t offset = 0;

    while (1) {
        wiz_claw_tg_poll(&tg_poll_cfg,
                          TG_POLL_TIMEOUT_SEC,
                          &offset,
                          on_telegram_message,
                          &agent_ctx);
    }
}
