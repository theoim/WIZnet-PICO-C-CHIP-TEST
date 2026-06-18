/*
 * wiz_claw_webserver.c — W55RP20 설정 웹서버 구현
 *
 * GET  /      → 설정 페이지 HTML 반환
 * POST /save  → 폼 파싱 → flash 저장 → 3초 후 watchdog 리부트
 *
 * 인증 없음 — 로컬 네트워크 전용.
 */
#include "wiz_claw_webserver.h"

#include "socket.h"            /* W5500 socket API */
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

_Static_assert(sizeof(wiz_claw_settings_t) == 1024,
               "wiz_claw_settings_t must be exactly 1024 bytes");
_Static_assert(1024 % FLASH_PAGE_SIZE == 0,
               "settings size must be multiple of FLASH_PAGE_SIZE");

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16u * 1024u * 1024u)  /* W55RP20 기본 16MB */
#endif

#define SETTINGS_FLASH_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SETTINGS_FLASH_XIP \
    ((const wiz_claw_settings_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET))

/* ── 기본값 ────────────────────────────────────────────────────── */

static const wiz_claw_settings_t s_defaults = {
    .magic         = WIZ_CLAW_WS_MAGIC,
    .ip            = {192, 168, 11, 20},
    .gw            = {192, 168, 11,  1},
    .sn            = {255, 255, 255, 0},
    .dns           = {8, 8, 8, 8},
    .dhcp          = 0,
    .llm_api_key   = "",
    .llm_base_url  = "https://gateway.ai.cloudflare.com",
    .llm_model     = "meta-llama/llama-4-scout-17b-16e-instruct",
    .llm_chat_path = "/v1/83f5e908134d951a5eecb420dc82a473"
                     "/esp-claw-theo/groq/chat/completions",
    .llm_max_tokens = "512",
    .tg_bot_token  = "",
    .vision_host   = "gateway.ai.cloudflare.com",
    .vision_path   = "/v1/83f5e908134d951a5eecb420dc82a473"
                     "/esp-claw-theo/groq/chat/completions",
};

static const wiz_claw_settings_t *s_cfg;

/* ── Flash 저장/로드 ───────────────────────────────────────────── */

void wiz_claw_settings_load(wiz_claw_settings_t *out)
{
    const wiz_claw_settings_t *flash = SETTINGS_FLASH_XIP;
    if (flash->magic == WIZ_CLAW_WS_MAGIC) {
        memcpy(out, flash, sizeof(*out));
        printf("[cfg] loaded from flash\n");
    } else {
        memcpy(out, &s_defaults, sizeof(*out));
        printf("[cfg] flash empty — using defaults\n");
    }
}

bool wiz_claw_settings_save(const wiz_claw_settings_t *cfg)
{
    wiz_claw_settings_t tmp;
    memcpy(&tmp, cfg, sizeof(tmp));
    tmp.magic = WIZ_CLAW_WS_MAGIC;

    /* Core 1 일시 정지 후 flash 조작 */
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET,
                        (const uint8_t *)&tmp, sizeof(tmp));
    restore_interrupts(ints);
    multicore_lockout_end_blocking();

    printf("[cfg] saved to flash (%zu bytes)\n", sizeof(tmp));
    return true;
}

/* ── URL 디코더 ────────────────────────────────────────────────── */

static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0;
    while (*src && i + 1 < dst_size) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* 폼 필드 추출 — "name=" 뒤 값을 URL 디코딩해서 out에 씀 */
static void form_field(const char *body, const char *name,
                        char *out, size_t out_size)
{
    char key[48];
    snprintf(key, sizeof(key), "%s=", name);
    size_t klen = strlen(key);

    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        /* 시작 위치 또는 '&' 바로 뒤여야 진짜 필드명 */
        if (p == body || *(p - 1) == '&') {
            p += klen;
            const char *end = strchr(p, '&');
            size_t raw_len = end ? (size_t)(end - p) : strlen(p);
            /* 후미 CR/LF/공백 제거 */
            while (raw_len > 0 && (unsigned char)p[raw_len - 1] < 0x20)
                raw_len--;
            char raw[512];
            if (raw_len >= sizeof(raw)) raw_len = sizeof(raw) - 1;
            memcpy(raw, p, raw_len);
            raw[raw_len] = '\0';
            url_decode(raw, out, out_size);
            return;
        }
        p++;
    }
    out[0] = '\0';
}

static void parse_ip(const char *str, uint8_t ip[4])
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d);
    ip[0] = (uint8_t)a; ip[1] = (uint8_t)b;
    ip[2] = (uint8_t)c; ip[3] = (uint8_t)d;
}

/* ── HTML 빌더 ─────────────────────────────────────────────────── */

#define HTML_BUF_SIZE 3200
static char s_html[HTML_BUF_SIZE];

static void build_config_html(const wiz_claw_settings_t *cfg)
{
    char ip[16], gw[16], sn[16], dns[16];
    snprintf(ip,  sizeof(ip),  "%u.%u.%u.%u",
             cfg->ip[0],  cfg->ip[1],  cfg->ip[2],  cfg->ip[3]);
    snprintf(gw,  sizeof(gw),  "%u.%u.%u.%u",
             cfg->gw[0],  cfg->gw[1],  cfg->gw[2],  cfg->gw[3]);
    snprintf(sn,  sizeof(sn),  "%u.%u.%u.%u",
             cfg->sn[0],  cfg->sn[1],  cfg->sn[2],  cfg->sn[3]);
    snprintf(dns, sizeof(dns), "%u.%u.%u.%u",
             cfg->dns[0], cfg->dns[1], cfg->dns[2], cfg->dns[3]);

    snprintf(s_html, HTML_BUF_SIZE,
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP-Claw Config</title>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{font-family:system-ui,sans-serif;background:#0d1117;color:#c9d1d9;"
              "padding:16px;margin:0}"
        ".w{max-width:540px;margin:auto}"
        "h1{color:#58a6ff;margin:0 0 2px;font-size:1.3em}"
        ".sub{color:#6e7681;font-size:.82em;margin-bottom:18px}"
        "h2{color:#79c0ff;font-size:.9em;border-bottom:1px solid #21262d;"
            "padding-bottom:5px;margin:18px 0 6px}"
        "label{font-size:.78em;color:#8b949e;display:block;margin-top:8px}"
        "input,select{width:100%%;background:#161b22;border:1px solid #30363d;"
                     "color:#c9d1d9;padding:6px 9px;border-radius:5px;"
                     "font-size:.88em;margin-top:2px}"
        "input:focus,select:focus{border-color:#58a6ff;outline:none}"
        ".r2{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
        ".sm{width:100px!important}"
        "button{margin-top:20px;width:100%%;padding:12px;background:#238636;"
               "color:#fff;border:none;border-radius:6px;font-size:.95em;"
               "font-weight:600;cursor:pointer}"
        "button:hover{background:#2ea043}"
        "</style></head><body><div class='w'>"
        "<h1>ESP-Claw</h1>"
        "<div class='sub'>W55RP20 Configuration — <a href='/' "
        "style='color:#58a6ff'>Refresh</a></div>"
        "<form method='POST' action='/save'>"

        /* Network */
        "<h2>Network (Static)</h2>"
        "<div class='r2'>"
        "<div><label>IP Address</label>"
             "<input name='ip' value='%s'></div>"
        "<div><label>Gateway</label>"
             "<input name='gw' value='%s'></div>"
        "<div><label>Subnet Mask</label>"
             "<input name='sn' value='%s'></div>"
        "<div><label>DNS</label>"
             "<input name='dns' value='%s'></div>"
        "</div>"

        /* LLM */
        "<h2>LLM (OpenAI Compatible)</h2>"
        "<label>API Key</label>"
        "<input type='password' name='llm_key' value='%s' autocomplete='off'>"
        "<label>Base URL</label>"
        "<input name='llm_url' value='%s'>"
        "<label>Model</label>"
        "<input name='llm_model' value='%s'>"
        "<label>Chat Path</label>"
        "<input name='llm_path' value='%s'>"
        "<label>Max Tokens</label>"
        "<input class='sm' name='llm_tokens' value='%s'>"

        /* Telegram */
        "<h2>Telegram</h2>"
        "<label>Bot Token</label>"
        "<input type='password' name='tg_token' value='%s' autocomplete='off'>"

        /* Vision */
        "<h2>Vision API</h2>"
        "<label>Host</label>"
        "<input name='vis_host' value='%s'>"
        "<label>Path</label>"
        "<input name='vis_path' value='%s'>"

        "<button type='submit'>Save &amp; Reboot</button>"
        "</form></div></body></html>",

        /* network */
        ip, gw, sn, dns,
        /* llm */
        cfg->llm_api_key, cfg->llm_base_url, cfg->llm_model,
        cfg->llm_chat_path, cfg->llm_max_tokens,
        /* telegram */
        cfg->tg_bot_token,
        /* vision */
        cfg->vision_host, cfg->vision_path
    );
}

static const char REBOOT_HTML[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='5;url=/'>"
    "<title>Saved</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#0d1117;color:#c9d1d9;"
         "display:flex;align-items:center;justify-content:center;"
         "height:100vh;margin:0;text-align:center}"
    "h1{color:#3fb950;font-size:2.2em;margin-bottom:8px}"
    "p{color:#6e7681;margin:4px 0}"
    "</style></head><body>"
    "<div>"
    "<h1>&#x2713; Saved</h1>"
    "<p>Rebooting in 3 seconds...</p>"
    "<p style='font-size:.8em'>Page reloads automatically.</p>"
    "</div></body></html>";

/* ── 소켓 송신 (1KB 청크) ──────────────────────────────────────── */

static void ws_send(uint8_t sn, const char *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        uint16_t chunk = (len > 1024u) ? 1024u : (uint16_t)len;
        int32_t  r     = send(sn, (uint8_t *)p, chunk);
        if (r <= 0) { break; }
        p   += (size_t)r;
        len -= (size_t)r;
    }
}

/* ── 요청 처리 ─────────────────────────────────────────────────── */

#define RX_BUF_SIZE 2048
static uint8_t s_rx[RX_BUF_SIZE];

static void handle_request(uint8_t sn)
{
    int32_t n = recv(sn, s_rx, RX_BUF_SIZE - 1);
    if (n <= 0) { return; }
    s_rx[n] = '\0';

    char *req = (char *)s_rx;
    printf("[webserver] %d bytes: %.60s\n", (int)n, req);

    if (strncmp(req, "GET ", 4) == 0) {
        build_config_html(s_cfg);
        ws_send(sn, s_html, strlen(s_html));
        printf("[webserver] config page served (%zu bytes)\n", strlen(s_html));
        disconnect(sn);
        close(sn);
        return;
    }

    if (strncmp(req, "POST /save", 10) == 0) {
        /* body는 \r\n\r\n 이후 */
        char *body = strstr(req, "\r\n\r\n");
        if (!body) {
            /* 헤더가 잘린 경우 추가 수신 시도 */
            int32_t more = recv(sn, s_rx + n,
                                (uint16_t)(RX_BUF_SIZE - 1 - (size_t)n));
            if (more > 0) {
                n += more;
                s_rx[n] = '\0';
                body = strstr(req, "\r\n\r\n");
            }
        }
        if (!body) {
            const char *e = "HTTP/1.0 400 Bad Request\r\n\r\nBad request";
            ws_send(sn, e, strlen(e));
            disconnect(sn);
            close(sn);
            return;
        }
        body += 4;

        /* 현재 설정을 복사 후 폼 필드로 덮어씀 */
        wiz_claw_settings_t ns;
        memcpy(&ns, s_cfg, sizeof(ns));

        char tmp[256];

        ns.dhcp = 0;

        form_field(body, "ip",  tmp, sizeof(tmp)); parse_ip(tmp, ns.ip);
        form_field(body, "gw",  tmp, sizeof(tmp)); parse_ip(tmp, ns.gw);
        form_field(body, "sn",  tmp, sizeof(tmp)); parse_ip(tmp, ns.sn);
        form_field(body, "dns", tmp, sizeof(tmp)); parse_ip(tmp, ns.dns);

        form_field(body, "llm_key",    ns.llm_api_key,    sizeof(ns.llm_api_key));
        form_field(body, "llm_url",    ns.llm_base_url,   sizeof(ns.llm_base_url));
        form_field(body, "llm_model",  ns.llm_model,      sizeof(ns.llm_model));
        form_field(body, "llm_path",   ns.llm_chat_path,  sizeof(ns.llm_chat_path));
        form_field(body, "llm_tokens", ns.llm_max_tokens, sizeof(ns.llm_max_tokens));
        form_field(body, "tg_token",   ns.tg_bot_token,   sizeof(ns.tg_bot_token));
        form_field(body, "vis_host",   ns.vision_host,    sizeof(ns.vision_host));
        form_field(body, "vis_path",   ns.vision_path,    sizeof(ns.vision_path));

        wiz_claw_settings_save(&ns);

        ws_send(sn, REBOOT_HTML, sizeof(REBOOT_HTML) - 1);
        disconnect(sn);
        close(sn);

        printf("[webserver] settings saved — rebooting in 3s\n");
        sleep_ms(3000);
        watchdog_enable(100, 1);
        while (1) tight_loop_contents();
    }

    /* favicon.ico 등 나머지 → 404 */
    const char *not_found =
        "HTTP/1.0 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n\r\n"
        "Not found";
    ws_send(sn, not_found, strlen(not_found));
    disconnect(sn);
    close(sn);
}

/* ── Public API ────────────────────────────────────────────────── */

void wiz_claw_webserver_init(const wiz_claw_settings_t *settings)
{
    s_cfg = settings;
    if (getSn_SR(WIZ_CLAW_WS_SOCK) != SOCK_CLOSED) {
        close(WIZ_CLAW_WS_SOCK);
    }
    printf("[webserver] ready — http://%u.%u.%u.%u/\n",
           settings->ip[0], settings->ip[1],
           settings->ip[2], settings->ip[3]);
}

void wiz_claw_webserver_poll(void)
{
    uint8_t sr = getSn_SR(WIZ_CLAW_WS_SOCK);

    switch (sr) {
    case SOCK_CLOSED:
        socket(WIZ_CLAW_WS_SOCK, Sn_MR_TCP, WIZ_CLAW_WS_PORT, 0);
        listen(WIZ_CLAW_WS_SOCK);
        break;

    case SOCK_LISTEN:
        break;

    case SOCK_ESTABLISHED:
        if (getSn_RX_RSR(WIZ_CLAW_WS_SOCK) > 0) {
            handle_request(WIZ_CLAW_WS_SOCK);
        }
        break;

    case SOCK_CLOSE_WAIT:
        disconnect(WIZ_CLAW_WS_SOCK);
        break;

    default:
        break;
    }
}
