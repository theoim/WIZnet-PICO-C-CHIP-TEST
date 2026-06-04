/**
 * wiz_claw_http.c — HTTP/1.1 over WIZnet 하드웨어 TCP 소켓 (+ TLS 지원)
 *
 * 흐름 (HTTP):  URL 파싱 → DNS → TCP connect → 요청/응답 → 소켓 닫기
 * 흐름 (HTTPS): URL 파싱 → DNS → TCP connect → TLS 핸드셰이크 → 요청/응답 → TLS close
 *
 * 내부에서 wiz_http_transport_t 추상화 계층을 통해 TCP / TLS 두 경로를
 * 동일한 헤더·바디 파싱 코드가 처리합니다.
 */
#include "wiz_claw_http.h"
#include "wiz_claw_net.h"
#include "wiz_claw_tls.h"

#include "socket.h"      /* WIZnet socket API */
#include "pico/stdlib.h" /* sleep_ms */

#include <ctype.h>

static const char *TAG = "wiz_claw_http";

#define HTTP_DEFAULT_TIMEOUT_MS  30000u
#define HTTP_POLL_INTERVAL_MS    5u
#define HTTP_RECV_CHUNK          512u

/* ── URL 파서 ───────────────────────────────────────────────── */

wiz_claw_err_t wiz_claw_http_parse_url(const char            *url,
                                        wiz_claw_parsed_url_t *out)
{
    const char *p = url;
    const char *host_start;
    const char *path_start;
    const char *colon;
    size_t      host_len;

    if (!url || !out) { return WIZ_CLAW_ERR_INVALID_ARG; }
    memset(out, 0, sizeof(*out));

    /* scheme */
    if (strncmp(p, "https://", 8) == 0) {
        out->is_https = true;
        out->port     = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        out->is_https = false;
        out->port     = 80;
        p += 7;
    } else {
        WIZ_CLAW_LOG_E(TAG, "unsupported scheme: %.32s", url);
        return WIZ_CLAW_ERR_INVALID_ARG;
    }

    host_start = p;
    path_start = strchr(p, '/');
    if (!path_start) { path_start = p + strlen(p); }

    colon = (const char *)memchr(host_start, ':', (size_t)(path_start - host_start));
    if (colon) {
        host_len  = (size_t)(colon - host_start);
        out->port = (uint16_t)atoi(colon + 1);
    } else {
        host_len  = (size_t)(path_start - host_start);
    }

    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return WIZ_CLAW_ERR_INVALID_ARG;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (*path_start == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else {
        size_t plen = strlen(path_start);
        if (plen >= sizeof(out->path)) { plen = sizeof(out->path) - 1; }
        memcpy(out->path, path_start, plen);
        out->path[plen] = '\0';
    }

    return WIZ_CLAW_OK;
}

/* ── TCP 헬퍼 ───────────────────────────────────────────────── */

wiz_claw_err_t wiz_claw_tcp_connect(uint8_t        sn,
                                     const uint8_t  ip[4],
                                     uint16_t       port,
                                     uint32_t       timeout_ms)
{
    int8_t   ret;
    uint32_t waited = 0;

    if (timeout_ms == 0) { timeout_ms = HTTP_DEFAULT_TIMEOUT_MS; }

    if (getSn_SR(sn) != SOCK_CLOSED) {
        disconnect(sn);
        close(sn);
        sleep_ms(10);
    }

    ret = socket(sn, Sn_MR_TCP, 0, 0);
    if (ret != (int8_t)sn) {
        WIZ_CLAW_LOG_E(TAG, "socket(%u) failed ret=%d", sn, ret);
        return WIZ_CLAW_ERR_HTTP;
    }

    ret = connect(sn, (uint8_t *)ip, port);
    if (ret != SOCK_OK) {
        WIZ_CLAW_LOG_E(TAG, "connect(%u) to %u.%u.%u.%u:%u failed ret=%d",
                        sn, ip[0], ip[1], ip[2], ip[3], port, ret);
        close(sn);
        return WIZ_CLAW_ERR_HTTP;
    }

    while (getSn_SR(sn) != SOCK_ESTABLISHED) {
        uint8_t sr = getSn_SR(sn);
        if (sr == SOCK_CLOSED || sr == SOCK_CLOSE_WAIT) {
            WIZ_CLAW_LOG_E(TAG, "connect aborted sr=0x%02x", sr);
            close(sn);
            return WIZ_CLAW_ERR_HTTP;
        }
        sleep_ms(HTTP_POLL_INTERVAL_MS);
        waited += HTTP_POLL_INTERVAL_MS;
        if (waited >= timeout_ms) {
            WIZ_CLAW_LOG_E(TAG, "connect timeout sn=%u", sn);
            disconnect(sn);
            close(sn);
            return WIZ_CLAW_ERR_HTTP;
        }
    }

    WIZ_CLAW_LOG_I(TAG, "TCP connected sn=%u %u.%u.%u.%u:%u",
                    sn, ip[0], ip[1], ip[2], ip[3], port);
    return WIZ_CLAW_OK;
}

void wiz_claw_tcp_close(uint8_t sn)
{
    if (getSn_SR(sn) != SOCK_CLOSED) {
        disconnect(sn);
        sleep_ms(10);
        close(sn);
    }
}

wiz_claw_err_t wiz_claw_tcp_send_all(uint8_t        sn,
                                      const uint8_t *data,
                                      size_t         len,
                                      uint32_t       timeout_ms)
{
    size_t   sent  = 0;
    uint32_t waited = 0;
    int32_t  ret;

    if (timeout_ms == 0) { timeout_ms = HTTP_DEFAULT_TIMEOUT_MS; }

    while (sent < len) {
        uint16_t to_send = (uint16_t)((len - sent) > 1460 ? 1460 : (len - sent));
        ret = send(sn, (uint8_t *)data + sent, to_send);
        if (ret < 0) {
            WIZ_CLAW_LOG_E(TAG, "send error ret=%d", ret);
            return WIZ_CLAW_ERR_HTTP;
        }
        if (ret == 0) {
            sleep_ms(HTTP_POLL_INTERVAL_MS);
            waited += HTTP_POLL_INTERVAL_MS;
            if (waited >= timeout_ms) {
                WIZ_CLAW_LOG_E(TAG, "send timeout");
                return WIZ_CLAW_ERR_HTTP;
            }
            continue;
        }
        sent  += (size_t)ret;
        waited = 0;
    }
    return WIZ_CLAW_OK;
}

int32_t wiz_claw_tcp_recv_wait(uint8_t   sn,
                                uint8_t  *buf,
                                uint16_t  len,
                                uint32_t  timeout_ms)
{
    uint32_t waited = 0;
    int32_t  ret;

    if (timeout_ms == 0) { timeout_ms = HTTP_DEFAULT_TIMEOUT_MS; }

    while (1) {
        if (getSn_RX_RSR(sn) > 0) {
            ret = recv(sn, buf, len);
            if (ret > 0) { return ret; }
            if (ret < 0) {
                WIZ_CLAW_LOG_E(TAG, "recv error ret=%d", ret);
                return ret;
            }
        }
        uint8_t sr = getSn_SR(sn);
        if (sr == SOCK_CLOSE_WAIT || sr == SOCK_CLOSED) {
            if (getSn_RX_RSR(sn) == 0) { return 0; }
        }
        sleep_ms(HTTP_POLL_INTERVAL_MS);
        waited += HTTP_POLL_INTERVAL_MS;
        if (waited >= timeout_ms) {
            WIZ_CLAW_LOG_E(TAG, "recv timeout sn=%u", sn);
            return -1;
        }
    }
}

/* ── 내부 전송 추상화 ───────────────────────────────────────────
 *
 * TCP / TLS 두 경로를 동일한 헤더·바디 파싱 함수로 처리하기 위한
 * 최소한의 인터페이스. static 전용이므로 헤더에 노출하지 않음.
 */
typedef struct {
    bool             is_tls;
    uint8_t          socket_no;
    wiz_claw_tls_t  *tls;   /* is_tls == true 일 때만 유효 */
    uint32_t         timeout_ms;
} wiz_http_transport_t;

static int32_t transport_recv(wiz_http_transport_t *t,
                               uint8_t *buf, uint16_t len)
{
    if (t->is_tls) {
        return wiz_claw_tls_read(t->tls, buf, len, t->timeout_ms);
    }
    return wiz_claw_tcp_recv_wait(t->socket_no, buf, len, t->timeout_ms);
}

static wiz_claw_err_t transport_send(wiz_http_transport_t *t,
                                      const uint8_t *data, size_t len)
{
    if (t->is_tls) {
        int32_t ret = wiz_claw_tls_write(t->tls, data, len);
        return (ret >= 0) ? WIZ_CLAW_OK : WIZ_CLAW_ERR_HTTP;
    }
    return wiz_claw_tcp_send_all(t->socket_no, data, len, t->timeout_ms);
}

static void transport_close(wiz_http_transport_t *t)
{
    if (t->is_tls) {
        wiz_claw_tls_close(t->tls);
        wiz_claw_tls_free(t->tls);
    } else {
        wiz_claw_tcp_close(t->socket_no);
    }
}

/* ── HTTP 내부 파싱 ─────────────────────────────────────────── */

static wiz_claw_err_t recv_headers(wiz_http_transport_t *t,
                                    char                 *hbuf,
                                    size_t                hbuf_size,
                                    size_t               *out_hlen)
{
    size_t  pos = 0;
    uint8_t c;
    int32_t ret;

    while (pos < hbuf_size - 1) {
        ret = transport_recv(t, &c, 1);
        if (ret <= 0) {
            WIZ_CLAW_LOG_E(TAG, "header recv failed ret=%d", ret);
            return WIZ_CLAW_ERR_HTTP;
        }
        hbuf[pos++] = (char)c;
        hbuf[pos]   = '\0';

        if (pos >= 4 &&
            hbuf[pos-4] == '\r' && hbuf[pos-3] == '\n' &&
            hbuf[pos-2] == '\r' && hbuf[pos-1] == '\n') {
            *out_hlen = pos;
            return WIZ_CLAW_OK;
        }
    }
    WIZ_CLAW_LOG_E(TAG, "header too large (>%zu)", hbuf_size);
    return WIZ_CLAW_ERR_HTTP;
}

static const char *find_header_value(const char *headers, const char *name)
{
    const char *p    = headers;
    size_t      nlen = strlen(name);

    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        size_t i;
        for (i = 0; i < nlen; i++) {
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)name[i])) {
                break;
            }
        }
        if (i == nlen && p[nlen] == ':') {
            const char *val = p + nlen + 1;
            while (*val == ' ') { val++; }
            return val;
        }
    }
    return NULL;
}

static int parse_status_code(const char *headers)
{
    const char *p = strchr(headers, ' ');
    if (!p) { return 0; }
    return atoi(p + 1);
}

static wiz_claw_err_t recv_chunked_body(wiz_http_transport_t *t,
                                         char                **inout_buf,
                                         size_t               *inout_cap,
                                         size_t               *out_len)
{
    char    line[32];
    size_t  line_pos = 0;
    size_t  total    = 0;
    uint8_t c;
    int32_t ret;

    *out_len = 0;

    while (1) {
        line_pos = 0;
        while (line_pos < sizeof(line) - 1) {
            ret = transport_recv(t, &c, 1);
            if (ret <= 0) { return WIZ_CLAW_ERR_HTTP; }
            if (c == '\n') { break; }
            if (c != '\r') { line[line_pos++] = (char)c; }
        }
        line[line_pos] = '\0';

        size_t chunk_size = (size_t)strtoul(line, NULL, 16);
        if (chunk_size == 0) { break; }

        if (total + chunk_size + 1 > *inout_cap) {
            size_t new_cap = total + chunk_size + WIZ_CLAW_HTTP_BODY_INIT;
            if (new_cap > WIZ_CLAW_HTTP_BODY_MAX) {
                WIZ_CLAW_LOG_E(TAG, "body > max (%u)", WIZ_CLAW_HTTP_BODY_MAX);
                return WIZ_CLAW_ERR_NO_MEM;
            }
            char *nb = (char *)realloc(*inout_buf, new_cap);
            if (!nb) { return WIZ_CLAW_ERR_NO_MEM; }
            *inout_buf = nb;
            *inout_cap = new_cap;
        }

        size_t received = 0;
        while (received < chunk_size) {
            uint16_t want = (uint16_t)(chunk_size - received < HTTP_RECV_CHUNK
                                       ? chunk_size - received : HTTP_RECV_CHUNK);
            ret = transport_recv(t, (uint8_t *)(*inout_buf) + total + received, want);
            if (ret <= 0) { return WIZ_CLAW_ERR_HTTP; }
            received += (size_t)ret;
        }
        total += chunk_size;

        transport_recv(t, &c, 1); /* \r */
        transport_recv(t, &c, 1); /* \n */
    }

    (*inout_buf)[total] = '\0';
    *out_len = total;
    return WIZ_CLAW_OK;
}

static wiz_claw_err_t recv_body_by_length(wiz_http_transport_t *t,
                                           char                **inout_buf,
                                           size_t               *inout_cap,
                                           size_t                content_len,
                                           size_t               *out_len)
{
    size_t  received = 0;
    int32_t ret;

    if (content_len + 1 > *inout_cap) {
        if (content_len >= WIZ_CLAW_HTTP_BODY_MAX) {
            WIZ_CLAW_LOG_E(TAG, "Content-Length %zu > max", content_len);
            return WIZ_CLAW_ERR_NO_MEM;
        }
        char *nb = (char *)realloc(*inout_buf, content_len + 1);
        if (!nb) { return WIZ_CLAW_ERR_NO_MEM; }
        *inout_buf = nb;
        *inout_cap = content_len + 1;
    }

    while (received < content_len) {
        uint16_t want = (uint16_t)(content_len - received < HTTP_RECV_CHUNK
                                   ? content_len - received : HTTP_RECV_CHUNK);
        ret = transport_recv(t, (uint8_t *)(*inout_buf) + received, want);
        if (ret < 0) { return WIZ_CLAW_ERR_HTTP; }
        if (ret == 0) { break; }
        received += (size_t)ret;
    }

    (*inout_buf)[received] = '\0';
    *out_len = received;
    return WIZ_CLAW_OK;
}

/* ── 핵심 요청 실행 ─────────────────────────────────────────── */

/* HTTPS용 정적 TLS 컨텍스트 — 한 번에 하나의 연결만 지원 (단일 스레드 설계) */
static wiz_claw_tls_t s_tls;

static wiz_claw_err_t do_http_request(uint8_t     sn,
                                       const char *method,
                                       const char *url,
                                       const char *auth_header,
                                       const char *extra_headers,
                                       const char *body,
                                       char      **out_response,
                                       uint32_t    timeout_ms)
{
    wiz_claw_parsed_url_t parsed;
    uint8_t               ip[4];
    char                 *req_buf  = NULL;
    char                 *hdr_buf  = NULL;
    char                 *body_buf = NULL;
    size_t                body_cap = WIZ_CLAW_HTTP_BODY_INIT;
    size_t                body_len = 0;
    size_t                hdr_len  = 0;
    int                   status;
    wiz_claw_err_t        err;
    bool                  tls_inited = false;

    if (timeout_ms == 0) { timeout_ms = HTTP_DEFAULT_TIMEOUT_MS; }

    /* 1. URL 파싱 */
    err = wiz_claw_http_parse_url(url, &parsed);
    if (err != WIZ_CLAW_OK) { goto done; }

    /* 2. DNS 해석 */
    err = wiz_claw_net_dns_resolve(parsed.host, ip);
    if (err != WIZ_CLAW_OK) { goto done; }

    /* 3. 전송 계층 초기화 및 연결 */
    wiz_http_transport_t transport = {
        .is_tls    = parsed.is_https,
        .socket_no = sn,
        .tls       = parsed.is_https ? &s_tls : NULL,
        .timeout_ms = timeout_ms,
    };

    if (parsed.is_https) {
        err = wiz_claw_tls_init(&s_tls, sn, timeout_ms);
        if (err != WIZ_CLAW_OK) { goto done; }
        tls_inited = true;
        err = wiz_claw_tls_connect(&s_tls, ip, parsed.port, parsed.host, timeout_ms);
    } else {
        err = wiz_claw_tcp_connect(sn, ip, parsed.port, timeout_ms);
    }
    if (err != WIZ_CLAW_OK) { goto done_tls; }

    /* 4. 요청 헤더 빌드 및 전송 */
    size_t body_bytes = body ? strlen(body) : 0;
    size_t req_size   = 512 + (auth_header    ? strlen(auth_header)    : 0)
                             + (extra_headers  ? strlen(extra_headers)  : 0)
                             + strlen(parsed.host) + strlen(parsed.path);

    req_buf = (char *)malloc(req_size);
    if (!req_buf) { err = WIZ_CLAW_ERR_NO_MEM; goto close; }

    int req_len = snprintf(req_buf, req_size,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "%s"
        "%s"
        "Content-Length: %zu\r\n"
        "\r\n",
        method, parsed.path, parsed.host,
        auth_header   ? auth_header   : "",
        extra_headers ? extra_headers : "",
        body_bytes);

    err = transport_send(&transport, (uint8_t *)req_buf, (size_t)req_len);
    if (err != WIZ_CLAW_OK) { goto close; }

    if (body && body_bytes > 0) {
        err = transport_send(&transport, (uint8_t *)body, body_bytes);
        if (err != WIZ_CLAW_OK) { goto close; }
    }

    /* 5. 응답 헤더 수신 */
    hdr_buf = (char *)malloc(WIZ_CLAW_HTTP_HEADER_BUF);
    if (!hdr_buf) { err = WIZ_CLAW_ERR_NO_MEM; goto close; }

    err = recv_headers(&transport, hdr_buf, WIZ_CLAW_HTTP_HEADER_BUF, &hdr_len);
    if (err != WIZ_CLAW_OK) { goto close; }

    /* 6. 상태 코드 */
    status = parse_status_code(hdr_buf);
    WIZ_CLAW_LOG_I(TAG, "HTTP %s %s → %d (%s)",
                   method, parsed.path, status,
                   parsed.is_https ? "TLS" : "plain");

    /* 7. 바디 수신 */
    body_buf = (char *)malloc(body_cap);
    if (!body_buf) { err = WIZ_CLAW_ERR_NO_MEM; goto close; }

    const char *te = find_header_value(hdr_buf, "transfer-encoding");
    const char *cl = find_header_value(hdr_buf, "content-length");

    if (te && strncasecmp(te, "chunked", 7) == 0) {
        err = recv_chunked_body(&transport, &body_buf, &body_cap, &body_len);
    } else if (cl) {
        size_t clen = (size_t)atoi(cl);
        err = recv_body_by_length(&transport, &body_buf, &body_cap, clen, &body_len);
    } else {
        while (1) {
            if (body_len + HTTP_RECV_CHUNK + 1 > body_cap) {
                size_t new_cap = body_cap + WIZ_CLAW_HTTP_BODY_INIT;
                if (new_cap > WIZ_CLAW_HTTP_BODY_MAX) { break; }
                char *nb = (char *)realloc(body_buf, new_cap);
                if (!nb) { break; }
                body_buf = nb;
                body_cap = new_cap;
            }
            int32_t got = transport_recv(&transport,
                                          (uint8_t *)body_buf + body_len,
                                          HTTP_RECV_CHUNK);
            if (got <= 0) { break; }
            body_len += (size_t)got;
        }
        body_buf[body_len] = '\0';
        err = WIZ_CLAW_OK;
    }

    if (err == WIZ_CLAW_OK && out_response) {
        *out_response = body_buf;
        body_buf      = NULL;
    }

close:
    transport_close(&transport);
    tls_inited = false;

done_tls:
    if (tls_inited) {
        wiz_claw_tls_free(&s_tls);
    }

done:
    free(req_buf);
    free(hdr_buf);
    free(body_buf);
    return err;
}

/* ── 공개 콜백 함수 ─────────────────────────────────────────── */

wiz_claw_err_t wiz_claw_http_post_cb(const char *url,
                                      const char *auth_header,
                                      const char *body_json,
                                      char      **out_response,
                                      void       *user_ctx)
{
    wiz_claw_http_ctx_t *ctx = (wiz_claw_http_ctx_t *)user_ctx;
    char auth_line[256] = {0};

    if (!ctx) { return WIZ_CLAW_ERR_INVALID_ARG; }

    if (auth_header && auth_header[0]) {
        snprintf(auth_line, sizeof(auth_line), "Authorization: %s\r\n", auth_header);
    }

    return do_http_request(ctx->socket_no,
                           "POST",
                           url,
                           auth_header ? auth_line : NULL,
                           NULL,
                           body_json,
                           out_response,
                           ctx->timeout_ms);
}

wiz_claw_err_t wiz_claw_http_call_cb(const char *url,
                                      const char *body_json,
                                      char      **out_response,
                                      void       *user_ctx)
{
    wiz_claw_http_ctx_t *ctx = (wiz_claw_http_ctx_t *)user_ctx;
    const char *method = (body_json && body_json[0]) ? "POST" : "GET";

    if (!ctx) { return WIZ_CLAW_ERR_INVALID_ARG; }

    return do_http_request(ctx->socket_no,
                           method,
                           url,
                           NULL,
                           NULL,
                           body_json,
                           out_response,
                           ctx->timeout_ms);
}
