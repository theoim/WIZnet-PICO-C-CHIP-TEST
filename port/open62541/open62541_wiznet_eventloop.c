#include "open62541.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "socket.h"
#include "wizchip_conf.h"

#include "wiznet_eventloop_config.h"

/* 동시에 LISTEN 시킬 하드웨어 소켓 개수.
 *
 * 컴파일 상수가 아니라 런타임 값이다. OPEN62541_WIZNET 은 모든 예제가
 * 공유하는 정적 라이브러리라, 예제별 compile definition 으로는 값이 안 갈린다.
 * UA_EventLoop_LWIP_setSocketCount() 로 서버 시작 전에 지정한다.
 *
 * 기본 1 — 아무것도 호출하지 않으면 기존 동작 그대로다.
 * opcua_usb_stdio 는 회귀 비교 기준이므로 손대지 않는다.
 *
 * W6300 이 같은 포트로 여러 소켓을 동시에 LISTEN 시킬 수 있다는 것은
 * 2026-09-03 실기로 확인했다. 소켓 버퍼 배분은 애플리케이션이 wizchip
 * 초기화 때 정한다 (examples/opcua_node/src/wiznet_network.c 의 memsize). */
#define WIZ_UA_SOCKET_MAX 8u

#ifndef WIZ_UA_SOCKET_COUNT_DEFAULT
#define WIZ_UA_SOCKET_COUNT_DEFAULT 1u
#endif

static uint8_t g_wizSocketCount = WIZ_UA_SOCKET_COUNT_DEFAULT;

/* OPC UA 가 쓰는 첫 하드웨어 소켓 번호. 이후 COUNT 개를 연속으로 점유한다. */
#ifndef WIZ_UA_SOCKET_BASE
#define WIZ_UA_SOCKET_BASE 0u
#endif

#define WIZ_SOCKET_OF(idx) ((uint8_t)(WIZ_UA_SOCKET_BASE + (idx)))

#define WIZ_OPCUA_TCP_PORT 4840u

/* 연결 ID 공간. 소켓 번호에서 유도한다.
 *   10..19  listen   (지금은 논리 listener 1개만 쓴다)
 *   20..29  active   소켓 인덱스별
 *   30..    예약 — 나중에 HTTP 설정 서버 등이 같은 이벤트루프에 들어올 자리 */
#define WIZ_LISTEN_ID_BASE 10u
#define WIZ_ACTIVE_ID_BASE 20u
#define WIZ_LISTEN_CONNECTION_ID WIZ_LISTEN_ID_BASE
#define WIZ_ACTIVE_ID(idx) ((uintptr_t)(WIZ_ACTIVE_ID_BASE + (idx)))

void UA_EventLoop_LWIP_setSocketCount(uint8_t count) {
    if(count < 1u)
        count = 1u;
    if(count > WIZ_UA_SOCKET_MAX)
        count = WIZ_UA_SOCKET_MAX;
    g_wizSocketCount = count;
}

uint8_t UA_EventLoop_LWIP_getSocketCount(void) {
    return g_wizSocketCount;
}

/* 임시 진단. 멀티소켓 브링업이 끝나면 제거한다. */
#ifndef WIZ_EL_DEBUG
#define WIZ_EL_DEBUG 0
#endif
#if WIZ_EL_DEBUG
#define WIZ_DBG(...) printf("[el] " __VA_ARGS__)
#else
#define WIZ_DBG(...) do {} while(0)
#endif

#define WIZ_RX_BUFFER_SIZE 8192u
#define WIZ_TIMER_MAX 32u

typedef struct {
    bool active;
    UA_UInt64 id;
    UA_Callback callback;
    void *application;
    void *context;
    UA_DateTime nextTime;
    UA_Double intervalMs;
    UA_TimerPolicy policy;
} WizTimerSlot;

typedef struct {
    UA_EventLoop eventLoop;
    UA_DelayedCallback *delayedCallbacks;
    WizTimerSlot timers[WIZ_TIMER_MAX];
    UA_UInt64 nextTimerId;
} WizEventLoop;

/* 소켓 하나분의 연결 상태. 예전에는 이 필드들이 매니저에 직접 있었다. */
/* ── 핸드셰이크 어드미션 컨트롤 ───────────────────────────────────
 *
 * RSA SecureChannel 생성은 이 부품에서 비싸고 중단할 수 없다. 2026-10-15 실측
 * (RP2350 @150MHz, Basic256Sha256 SignAndEncrypt, 접속 1건당 약 1.66초):
 *
 *     동시 1개  1.706s   2개  3.357s   3개  5.034s ✗   4개  6.238s, 2개 실패
 *
 * 마지막 하나만 늦는 것이 아니라 전원이 전체 연산량을 함께 기다린다. 3개부터
 * 모두가 클라이언트 5초 타임아웃을 넘기고, 4개에서는 절반이 떨어진다.
 *
 * 더 나쁜 것은 실패한 뒤다. 클라이언트가 포기해도 서버는 CreateSession 까지
 * 끝낸 상태라 그 세션이 maxSessionTimeout(10분)까지 슬롯을 문다. 즉 재접속
 * 폭주는 그 순간 실패하고 끝나는 것이 아니라 이후 10분간 용량을 깎는다.
 *
 * ── 세는 대상: 클라이언트가 이미 기다린 시간 ──
 *
 * 여기까지 오는 데 모델을 두 번 버렸고, 둘 다 전달이 동기적이라는 사실에
 * 걸렸다. 소켓 0 의 OPN 을 서버에 넘기면 그 자리에서 RSA 가 끝나고 응답까지
 * 나간 뒤에야 콜백이 돌아온다.
 *
 *   "지금 진행 중인 핸드셰이크 수"  소켓 1 을 볼 때쯤 소켓 0 은 이미 끝나 있다.
 *                                   그 숫자는 영원히 0 아니면 1 이다.
 *   "서버 시간 예약 장부"            블로킹 1.7초 동안 장부도 같이 소진된다.
 *                                   매번 비어 있어서 아무도 못 막는다.
 *
 * 둘 다 놓친 것은 대기열이 우리 쪽 변수가 아니라 W6300 의 RX 버퍼에 있다는
 * 점이다. 버스트에서는 네 클라이언트의 OPN 이 거의 동시에 도착해 버퍼에
 * 앉아 있고, 우리는 그것을 1.7초 간격으로 하나씩 꺼내 읽는다. 읽은 시각만
 * 보면 언제나 서버는 한가해 보인다.
 *
 * 알 수 있는 것은 각 소켓이 언제 TCP 로 연결됐는지다. 그것이 그 클라이언트가
 * 실제로 기다린 시간이고, 거기에 핸드셰이크 비용을 더하면 그 클라이언트가
 * 답을 받을 시각이 나온다. 그 시각이 예산을 넘으면 **어차피 타임아웃될 일**
 * 이므로 시작하지 않는다.
 *
 *     c0  대기 0.0s + 1.7 = 1.7 ≤ 3.5 → 수락   c2  대기 3.4s + 1.7 = 5.1 → 거부
 *     c1  대기 1.7s + 1.7 = 3.4 ≤ 3.5 → 수락   c3  거부
 *
 * 거부는 TCP 를 끊는 것이고, 클라이언트는 빠르게 재시도할 수 있다. 받아놓고
 * 못 지키는 것보다 낫다 — 5초를 버리고 전원이 실패하며 유령 세션까지 남는다.
 *
 * 이것은 세션 용량이 아니다. 순차로 붙이면 1.69초로 일정하고 4개가 전부
 * 유지·응답한다. 제한은 "핸드셰이크 시작"에만 건다.
 *
 * 예산 0 이면 무제한 — A/B 측정용이다.
 */
#ifndef WIZ_HANDSHAKE_COST_MS
#define WIZ_HANDSHAKE_COST_MS   1700u   /* 실측 1.66s, 올림 */
#endif

/* 5초 클라이언트 타임아웃에서 왕복·인코딩·재시도 몫을 남긴 값이다.
 * 3,500 이면 2건까지 받고 3건째를 거절한다. */
#ifndef WIZ_HANDSHAKE_BUDGET_MS
#define WIZ_HANDSHAKE_BUDGET_MS 3500u
#endif

static uint32_t g_wizHandshakeRefused = 0;

/* OPC UA TCP 메시지 헤더는 3바이트 타입 + 1바이트 청크 표시로 시작한다.
 * SecureChannel 을 여는 것은 "OPN" 뿐이고, HEL/ACK 는 그 앞에서 끝난다. */
#define WIZ_IS_OPN(p, n)  ((n) >= 4u && (p)[0] == 'O' && (p)[1] == 'P' && (p)[2] == 'N')

/* 모든 OPN 이 비싼 것은 아니다.
 *
 * 클라이언트는 보안 접속 전에 SecurityPolicy#None 채널을 먼저 열어
 * GetEndpoints 를 한다(asyncua, UAExpert 모두 그렇다). 그 채널에는 비대칭
 * 연산이 없는데, 첫 구현은 OPN 이면 무조건 1.7초를 예약해서 **정작 보안
 * 핸드셰이크가 시작되기도 전에 예산을 다 써버렸다.** 로그에 거부로 찍힌 것이
 * 전부 GetEndpoints 채널이었다.
 *
 * OPN 본문은 헤더(8) + SecureChannelId(4) 다음에 SecurityPolicyUri 가
 * 길이 접두 문자열로 온다. 그 자리를 읽어 None 이면 공짜로 통과시킨다.
 * 못 읽으면 비싼 쪽으로 가정한다 — 과소 청구보다 과대 청구가 안전하다. */
static bool wiz_opn_needs_asymmetric(const uint8_t *p, size_t n) {
    static const char none_uri[] =
        "http://opcfoundation.org/UA/SecurityPolicy#None";
    const size_t none_len = sizeof(none_uri) - 1u;

    if(n < 16u)
        return true;

    uint32_t uriLen = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                      ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);

    if(uriLen == 0xFFFFFFFFu || uriLen > n - 16u)
        return true;

    if(uriLen == none_len && memcmp(p + 16, none_uri, none_len) == 0)
        return false;

    return true;
}

typedef struct {
    void *activeContext;
    bool activeAnnounced;
    bool pendingActiveClose;
    /* OPN 을 받아 서버에 넘겼고 아직 응답을 못 보낸 상태. 응답을 보내는
     * 순간 내려간다 — OPN 다음에 그 소켓으로 나가는 첫 송신이 OPN 응답이다. */
    bool handshaking;
    /* 이 연결에서 OPN 응답을 한 번 보냈다. 이후의 OPN 은 채널 갱신(renew)이다.
     * 갱신은 이미 붙어 있는 클라이언트가 수명마다 보내는 것이라 그 시점의
     * connectedAtMs 는 몇 분 전이고, 대기 시간으로 따지면 반드시 거부된다.
     * 갱신이 몰리는 일도 없다 — 클라이언트마다 접속 시각이 다르다. */
    bool channelOpened;
    /* TCP 가 연결된 시각. 이 클라이언트가 얼마나 기다렸는지를 알아야
     * "끝낼 수 없는 일을 시작하지 않는" 판단을 할 수 있다. */
    uint32_t connectedAtMs;
    uint8_t peerIp[4];
    uint16_t peerPort;
} WizSocketState;

typedef struct {
    UA_ConnectionManager cm;
    UA_ConnectionManager_connectionCallback callback;
    void *application;

    /* listen 은 논리적으로 하나다. 하드웨어 소켓이 여러 개여도
     * 상위에는 "포트 4840 을 듣고 있다" 하나로만 보인다. */
    void *listenContext;
    bool listenAnnounced;
    bool pendingListenClose;

    uint16_t port;
    WizSocketState s[WIZ_UA_SOCKET_MAX];

    /* recv() 직후 콜백에 넘겨 그 자리에서 소비되므로 소켓마다 둘 필요가 없다.
     * 소켓을 늘려도 RAM 은 그대로다. */
    uint8_t rxBuffer[WIZ_RX_BUFFER_SIZE];
} WizTcpConnectionManager;

/* ── 시계 ──────────────────────────────────────────────────────────
 * 두 시계를 반드시 분리해서 둔다.
 *
 * nowMonotonic 은 부팅 기준 단조 시간이고 UTC 오프셋을 절대 섞지 않는다.
 * open62541 은 이 값으로 구독 발행 주기, 세션 타임아웃, 재전송 타이머를
 * 돌린다. 여기에 시각 동기화 결과를 더하면 동기화가 성립하는 순간 시간이
 * 수십 년 앞으로 점프하고 **모든 타이머가 한꺼번에 만료된다.**
 *
 * now 는 벽시계다. 동기화 전에는 Unix epoch + 부팅 경과시간이므로 실제로는
 * 1970년이다. 그 상태를 정상 시각처럼 내보내지 않도록, 동기화 여부를
 * wiz_clock_is_synced() 로 노출하고 애플리케이션이 타임스탬프를 붙일지
 * 판단한다.
 *
 * 참고: 이 분리는 open62541 의 posix 구현과 같은 구조다 —
 * CLOCK_MONOTONIC_RAW(부팅 기준) 와 CLOCK_REALTIME(벽시계) 는 서로 다른
 * 값이며, 라이브러리는 둘을 섞어 비교하지 않는다. */

static bool       s_clock_synced;
static UA_DateTime s_clock_utc_base;  /* 동기화 시점의 UTC - 그 시점의 monotonic */

static UA_DateTime pico_monotonic(void) {
    return (UA_DateTime)(time_us_64() * UA_DATETIME_USEC);
}

UA_DateTime UA_DateTime_now(void) {
    if(!s_clock_synced)
        return UA_DATETIME_UNIX_EPOCH + pico_monotonic();
    return s_clock_utc_base + pico_monotonic();
}

UA_DateTime UA_DateTime_nowMonotonic(void) {
    return pico_monotonic();
}

UA_Int64 UA_DateTime_localTimeUtcOffset(void) {
    return 0;
}

void wiz_clock_set_utc(int64_t utc_now) {
    /* 오프셋만 갱신한다. monotonic 은 건드리지 않으므로 이미 걸려 있는
     * 타이머는 영향을 받지 않는다. */
    s_clock_utc_base = utc_now - pico_monotonic();
    s_clock_synced = true;
}

bool wiz_clock_is_synced(void) {
    return s_clock_synced;
}

static void set_eventloop_state(UA_EventLoop *el, UA_EventLoopState state) {
    *(UA_EventLoopState *)(uintptr_t)&el->state = state;
}

static void wiz_noop_lock(UA_EventLoop *el) {
    (void)el;
}

static void wiz_noop_unlock(UA_EventLoop *el) {
    (void)el;
}

static UA_StatusCode wiz_eventsource_start(UA_EventSource *es) {
    es->state = UA_EVENTSOURCESTATE_STARTED;
    return UA_STATUSCODE_GOOD;
}

static void wiz_eventsource_stop(UA_EventSource *es) {
    es->state = UA_EVENTSOURCESTATE_STOPPED;
}

static UA_StatusCode wiz_cm_free(UA_EventSource *es);
static void wiz_cm_poll(WizTcpConnectionManager *tcp);

static UA_StatusCode wiz_el_register_eventsource(UA_EventLoop *el,
                                                   UA_EventSource *es) {
    if(!el || !es)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    es->eventLoop = el;
    es->next = el->eventSources;
    el->eventSources = es;

    if(el->state == UA_EVENTLOOPSTATE_STARTED)
        return es->start(es);

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode wiz_el_deregister_eventsource(UA_EventLoop *el,
                                                     UA_EventSource *es) {
    if(!el || !es)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    if(es->state == UA_EVENTSOURCESTATE_STARTED)
        es->stop(es);

    UA_EventSource **cursor = &el->eventSources;
    while(*cursor) {
        if(*cursor == es) {
            *cursor = es->next;
            es->next = NULL;
            es->eventLoop = NULL;
            return UA_STATUSCODE_GOOD;
        }
        cursor = &(*cursor)->next;
    }

    return UA_STATUSCODE_BADNOTFOUND;
}

static UA_StatusCode wiz_el_start(UA_EventLoop *el) {
    if(!el)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    if(el->state == UA_EVENTLOOPSTATE_STARTED)
        return UA_STATUSCODE_GOOD;

    set_eventloop_state(el, UA_EVENTLOOPSTATE_STARTED);

    for(UA_EventSource *es = el->eventSources; es; es = es->next) {
        UA_StatusCode res = es->start(es);
        if(res != UA_STATUSCODE_GOOD)
            return res;
    }

    return UA_STATUSCODE_GOOD;
}

static void wiz_el_stop(UA_EventLoop *el) {
    if(!el)
        return;

    for(UA_EventSource *es = el->eventSources; es; es = es->next)
        es->stop(es);

    set_eventloop_state(el, UA_EVENTLOOPSTATE_STOPPED);
}

static UA_StatusCode wiz_el_free(UA_EventLoop *el) {
    if(!el)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    if(el->state == UA_EVENTLOOPSTATE_STARTED)
        wiz_el_stop(el);

    UA_EventSource *es = el->eventSources;
    while(es) {
        UA_EventSource *next = es->next;
        es->free(es);
        es = next;
    }

    UA_KeyValueMap_clear(&el->params);
    UA_free(el);
    return UA_STATUSCODE_GOOD;
}

static void wiz_run_delayed_callbacks(WizEventLoop *wel) {
    UA_DelayedCallback *dc = wel->delayedCallbacks;
    wel->delayedCallbacks = NULL;

    while(dc) {
        UA_DelayedCallback *next = dc->next;
        dc->next = NULL;
        if(dc->callback)
            dc->callback(dc->application, dc->context);
        dc = next;
    }
}

static void wiz_run_timers(WizEventLoop *wel) {
    UA_DateTime now = pico_monotonic();

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        WizTimerSlot *slot = &wel->timers[i];
        if(!slot->active || slot->nextTime > now)
            continue;

        UA_Callback cb = slot->callback;
        void *application = slot->application;
        void *context = slot->context;

        if(slot->policy == UA_TIMERPOLICY_ONCE) {
            slot->active = false;
        } else if(slot->policy == UA_TIMERPOLICY_BASETIME) {
            UA_DateTime interval =
                (UA_DateTime)(slot->intervalMs * (UA_Double)UA_DATETIME_MSEC);
            do {
                slot->nextTime += interval;
            } while(slot->nextTime <= now);
        } else {
            slot->nextTime = now +
                (UA_DateTime)(slot->intervalMs * (UA_Double)UA_DATETIME_MSEC);
        }

        if(cb)
            cb(application, context);
    }
}

static UA_StatusCode wiz_el_run(UA_EventLoop *el, UA_UInt32 timeout) {
    (void)timeout;
    WizEventLoop *wel = (WizEventLoop *)el;

    if(!el || el->state != UA_EVENTLOOPSTATE_STARTED)
        return UA_STATUSCODE_BADINTERNALERROR;

    wiz_run_delayed_callbacks(wel);
    wiz_run_timers(wel);

    for(UA_EventSource *es = el->eventSources; es; es = es->next) {
        if(es->eventSourceType != UA_EVENTSOURCETYPE_CONNECTIONMANAGER)
            continue;

        UA_ConnectionManager *cm = (UA_ConnectionManager *)es;
        if(cm->sendWithConnection)
            wiz_cm_poll((WizTcpConnectionManager *)cm);
    }

    return UA_STATUSCODE_GOOD;
}

static void wiz_el_cancel(UA_EventLoop *el) {
    (void)el;
}

/* 벽시계. 라이브러리는 이 값을 요청/응답 타임스탬프에 쓴다.
 * 동기화 전에는 1970년이라는 점을 애플리케이션이 알고 있어야 한다. */
static UA_DateTime wiz_el_now(UA_EventLoop *el) {
    (void)el;
    return UA_DateTime_now();
}

/* 단조 시계. 라이브러리는 이 값을 SecureChannel 토큰 수명, 세션 타임아웃,
 * 요청 타임아웃, 재전송에 쓴다. 벽시계와 반드시 분리해야 한다 —
 * 같은 함수로 두면 시각 동기화 순간 이 타이머들이 한꺼번에 만료된다. */
static UA_DateTime wiz_el_now_monotonic(UA_EventLoop *el) {
    (void)el;
    return pico_monotonic();
}

static UA_Int64 wiz_el_utc_offset(UA_EventLoop *el) {
    (void)el;
    return 0;
}

static UA_DateTime wiz_el_next_timer(UA_EventLoop *el) {
    WizEventLoop *wel = (WizEventLoop *)el;
    UA_DateTime next = UA_INT64_MAX;

    if(wel->delayedCallbacks)
        return pico_monotonic();

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        if(wel->timers[i].active && wel->timers[i].nextTime < next)
            next = wel->timers[i].nextTime;
    }

    return next;
}

static UA_StatusCode wiz_el_add_timer(UA_EventLoop *el, UA_Callback cb,
                                        void *application, void *data,
                                        UA_Double intervalMs,
                                        UA_DateTime *baseTime,
                                        UA_TimerPolicy timerPolicy,
                                        UA_UInt64 *timerId) {
    WizEventLoop *wel = (WizEventLoop *)el;

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        WizTimerSlot *slot = &wel->timers[i];
        if(slot->active)
            continue;

        UA_DateTime base = baseTime ? *baseTime : pico_monotonic();
        UA_DateTime interval =
            (UA_DateTime)(intervalMs * (UA_Double)UA_DATETIME_MSEC);

        slot->active = true;
        slot->id = ++wel->nextTimerId;
        slot->callback = cb;
        slot->application = application;
        slot->context = data;
        slot->intervalMs = intervalMs;
        slot->policy = timerPolicy;
        slot->nextTime = (timerPolicy == UA_TIMERPOLICY_ONCE && baseTime) ?
            base : base + interval;

        if(timerId)
            *timerId = slot->id;

        return UA_STATUSCODE_GOOD;
    }

    return UA_STATUSCODE_BADOUTOFMEMORY;
}

static UA_StatusCode wiz_el_modify_timer(UA_EventLoop *el,
                                           UA_UInt64 timerId,
                                           UA_Double intervalMs,
                                           UA_DateTime *baseTime,
                                           UA_TimerPolicy timerPolicy) {
    WizEventLoop *wel = (WizEventLoop *)el;

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        WizTimerSlot *slot = &wel->timers[i];
        if(!slot->active || slot->id != timerId)
            continue;

        UA_DateTime base = baseTime ? *baseTime : pico_monotonic();
        slot->intervalMs = intervalMs;
        slot->policy = timerPolicy;
        slot->nextTime = base +
            (UA_DateTime)(intervalMs * (UA_Double)UA_DATETIME_MSEC);
        return UA_STATUSCODE_GOOD;
    }

    return UA_STATUSCODE_BADNOTFOUND;
}

static void wiz_el_remove_timer(UA_EventLoop *el, UA_UInt64 timerId) {
    WizEventLoop *wel = (WizEventLoop *)el;

    for(size_t i = 0; i < WIZ_TIMER_MAX; i++) {
        if(wel->timers[i].active && wel->timers[i].id == timerId) {
            memset(&wel->timers[i], 0, sizeof(wel->timers[i]));
            return;
        }
    }
}

static void wiz_el_add_delayed(UA_EventLoop *el, UA_DelayedCallback *dc) {
    WizEventLoop *wel = (WizEventLoop *)el;

    dc->next = NULL;
    if(!wel->delayedCallbacks) {
        wel->delayedCallbacks = dc;
        return;
    }

    UA_DelayedCallback *tail = wel->delayedCallbacks;
    while(tail->next)
        tail = tail->next;
    tail->next = dc;
}

static void wiz_el_remove_delayed(UA_EventLoop *el, UA_DelayedCallback *dc) {
    WizEventLoop *wel = (WizEventLoop *)el;
    UA_DelayedCallback **cursor = &wel->delayedCallbacks;

    while(*cursor) {
        if(*cursor == dc) {
            *cursor = dc->next;
            dc->next = NULL;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

UA_EventLoop *UA_EventLoop_new_LWIP(const UA_Logger *logger,
                                    UA_EventLoopConfiguration *config) {
    (void)config;

    WizEventLoop *wel = (WizEventLoop *)UA_calloc(1, sizeof(WizEventLoop));
    if(!wel)
        return NULL;

    UA_EventLoop *el = &wel->eventLoop;
    el->logger = logger;
    set_eventloop_state(el, UA_EVENTLOOPSTATE_FRESH);
    el->start = wiz_el_start;
    el->stop = wiz_el_stop;
    el->free = wiz_el_free;
    el->run = wiz_el_run;
    el->cancel = wiz_el_cancel;
    el->dateTime_now = wiz_el_now;
    el->dateTime_nowMonotonic = wiz_el_now_monotonic;
    el->dateTime_localTimeUtcOffset = wiz_el_utc_offset;
    el->nextTimer = wiz_el_next_timer;
    el->addTimer = wiz_el_add_timer;
    el->modifyTimer = wiz_el_modify_timer;
    el->removeTimer = wiz_el_remove_timer;
    el->addDelayedCallback = wiz_el_add_delayed;
    el->removeDelayedCallback = wiz_el_remove_delayed;
    el->registerEventSource = wiz_el_register_eventsource;
    el->deregisterEventSource = wiz_el_deregister_eventsource;
    el->lock = wiz_noop_lock;
    el->unlock = wiz_noop_unlock;

    return el;
}

/* 한 번의 send() 로 보낼 수 있는 양은 소켓 TX 버퍼 크기(W5500 기본 2KB)가
 * 상한이므로, 그보다 큰 메시지는 반드시 여러 번에 나눠 보내야 한다.
 *
 * 이때 ioLibrary send() 의 반환값 0 을 실패로 보면 안 된다. 0 은 SOCK_BUSY
 * (socket.h) 로, "직전 SEND 가 아직 끝나지 않았으니 다시 부르라"는 뜻이다.
 * 실제 오류는 음수로만 온다 (SOCKERR_TIMEOUT, SOCKERR_SOCKSTATUS 등).
 *
 * 2026-09-10 W55RP20 실기: 채널 32개에서 Browse 응답 2,349 B 를 보내다
 * `send s0 FAIL rc=0 sent=2048/2349 sr=0x17` 로 연결이 끊겼다. 소켓 상태는
 * 0x17(ESTABLISHED) 로 정상이었고, 0 을 실패로 오판한 것이 원인이었다.
 *
 * 무한 대기를 막기 위해 "진행이 없는 시간"에만 상한을 둔다. 한 조각이라도
 * 나가면 기한을 다시 잡는다. W5500 자체 재전송 타임아웃(RTR/RCR 기본 약 1.8초)
 * 이 먼저 걸려 음수를 돌려주므로 그보다 넉넉한 값이면 충분하다. */
#define WIZ_SEND_STALL_TIMEOUT_MS 5000u

static int wiz_send_all(uint8_t sock, const uint8_t *buf, uint16_t len) {
    uint16_t sentTotal = 0u;
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) +
                        WIZ_SEND_STALL_TIMEOUT_MS;

    while(sentTotal < len) {
        int32_t sent = send(sock, (uint8_t *)buf + sentTotal,
                            (uint16_t)(len - sentTotal));

        if(sent < 0) {
            WIZ_DBG("send s%u FAIL rc=%ld sent=%u/%u sr=0x%02X\r\n",
                    sock, (long)sent, sentTotal, len, getSn_SR(sock));
            return -1;
        }

        if(sent == 0) {
            /* SOCK_BUSY — 칩이 앞 조각을 아직 내보내는 중이다. */
            if((int32_t)(to_ms_since_boot(get_absolute_time()) - deadline) >= 0) {
                WIZ_DBG("send s%u STALL sent=%u/%u sr=0x%02X\r\n",
                        sock, sentTotal, len, getSn_SR(sock));
                return -1;
            }
            continue;
        }

        sentTotal = (uint16_t)(sentTotal + (uint16_t)sent);
        deadline = to_ms_since_boot(get_absolute_time()) +
                   WIZ_SEND_STALL_TIMEOUT_MS;
    }

    return 0;
}

/* active 연결 ID -> 소켓 인덱스. 범위 밖이면 -1. */
static int wiz_index_of_active_id(uintptr_t connectionId) {
    if(connectionId < WIZ_ACTIVE_ID_BASE)
        return -1;
    uintptr_t idx = connectionId - WIZ_ACTIVE_ID_BASE;
    if(idx >= g_wizSocketCount)
        return -1;
    return (int)idx;
}

static void wiz_signal_listen(WizTcpConnectionManager *tcp) {
    if(tcp->listenAnnounced || !tcp->callback)
        return;

    UA_String listenAddress = UA_STRING_STATIC("0.0.0.0");
    UA_KeyValuePair params[2];
    params[0].key = UA_QUALIFIEDNAME(0, "listen-address");
    UA_Variant_setScalar(&params[0].value, &listenAddress,
                         &UA_TYPES[UA_TYPES_STRING]);
    params[1].key = UA_QUALIFIEDNAME(0, "listen-port");
    UA_Variant_setScalar(&params[1].value, &tcp->port,
                         &UA_TYPES[UA_TYPES_UINT16]);
    UA_KeyValueMap kvm = {2, params};

    tcp->callback(&tcp->cm, WIZ_LISTEN_CONNECTION_ID, tcp->application,
                  &tcp->listenContext, UA_CONNECTIONSTATE_ESTABLISHED,
                  &kvm, UA_BYTESTRING_NULL);
    tcp->listenAnnounced = true;
}

static void wiz_signal_active_open(WizTcpConnectionManager *tcp, unsigned idx) {
    WizSocketState *st = &tcp->s[idx];
    if(st->activeAnnounced || !tcp->callback)
        return;

    uint8_t sock = WIZ_SOCKET_OF(idx);
    getSn_DIPR(sock, st->peerIp);
    st->peerPort = getSn_DPORT(sock);
    /* The client's clock starts here, not when we get round to reading its
     * request. During a burst its bytes can sit in the chip's RX buffer for
     * seconds while earlier handshakes run, and that waiting is most of what
     * it experiences. */
    st->connectedAtMs = to_ms_since_boot(get_absolute_time());

    char remote[24];
    snprintf(remote, sizeof(remote), "%u.%u.%u.%u",
             st->peerIp[0], st->peerIp[1], st->peerIp[2], st->peerIp[3]);

    UA_String remoteAddress = UA_STRING(remote);
    UA_KeyValuePair kvp;
    kvp.key = UA_QUALIFIEDNAME(0, "remote-address");
    UA_Variant_setScalar(&kvp.value, &remoteAddress, &UA_TYPES[UA_TYPES_STRING]);
    UA_KeyValueMap kvm = {1, &kvp};

    st->activeContext = tcp->listenContext;
    tcp->callback(&tcp->cm, WIZ_ACTIVE_ID(idx), tcp->application,
                  &st->activeContext, UA_CONNECTIONSTATE_ESTABLISHED,
                  &kvm, UA_BYTESTRING_NULL);
    st->activeAnnounced = true;
    WIZ_DBG("open s%u id=%lu peer=%u.%u.%u.%u:%u\r\n",
            sock, (unsigned long)WIZ_ACTIVE_ID(idx),
            st->peerIp[0], st->peerIp[1], st->peerIp[2], st->peerIp[3],
            st->peerPort);
}

static void wiz_queue_active_close(WizTcpConnectionManager *tcp, unsigned idx) {
    tcp->s[idx].pendingActiveClose = true;
}

/* 소켓 하나의 미처리 close 를 반영한다. 상위에 CLOSING 을 통보하고 소켓을 닫는다.
 * 닫힌 소켓은 다음 폴링에서 wiz_open_listen_socket() 이 다시 LISTEN 으로 돌린다. */
static void wiz_process_pending_active_close(WizTcpConnectionManager *tcp,
                                             unsigned idx) {
    WizSocketState *st = &tcp->s[idx];
    if(!st->pendingActiveClose)
        return;

    st->pendingActiveClose = false;
    if(st->activeAnnounced && tcp->callback) {
        tcp->callback(&tcp->cm, WIZ_ACTIVE_ID(idx), tcp->application,
                      &st->activeContext, UA_CONNECTIONSTATE_CLOSING,
                      &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
    }
    WIZ_DBG("close s%u id=%lu\r\n", WIZ_SOCKET_OF(idx),
            (unsigned long)WIZ_ACTIVE_ID(idx));
    st->activeAnnounced = false;
    st->activeContext = NULL;
    /* A connection that dies mid-handshake must give the slot back, or the
     * limit ratchets down one refused client at a time until nothing connects. */
    st->handshaking = false;
    /* The next client on this socket starts fresh; its first OPN is a new
     * handshake, not a renewal. */
    st->channelOpened = false;
    close(WIZ_SOCKET_OF(idx));
}

/* listen 을 내리면 모든 소켓을 닫는다. */
static void wiz_process_pending_listen_close(WizTcpConnectionManager *tcp) {
    if(!tcp->pendingListenClose)
        return;

    tcp->pendingListenClose = false;
    if(tcp->listenAnnounced && tcp->callback) {
        tcp->callback(&tcp->cm, WIZ_LISTEN_CONNECTION_ID, tcp->application,
                      &tcp->listenContext, UA_CONNECTIONSTATE_CLOSING,
                      &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
    }
    tcp->listenAnnounced = false;
    tcp->listenContext = NULL;

    for(unsigned i = 0; i < g_wizSocketCount; i++)
        close(WIZ_SOCKET_OF(i));
}

static void wiz_open_listen_socket(WizTcpConnectionManager *tcp, unsigned idx) {
    uint8_t sock = WIZ_SOCKET_OF(idx);
    uint8_t sr = getSn_SR(sock);

    switch(sr) {
    case SOCK_CLOSED:
        /* 모든 소켓이 같은 포트로 열린다. W6300 에서 허용된다 (F-1). */
        if(socket(sock, Sn_MR_TCP, tcp->port, SF_TCP_NODELAY) < 0)
            return;
        break;

    case SOCK_INIT:
        if(listen(sock) == SOCK_OK)
            wiz_signal_listen(tcp);
        break;

    case SOCK_LISTEN:
        wiz_signal_listen(tcp);
        break;

    default:
        break;
    }
}

static void wiz_cm_poll_socket(WizTcpConnectionManager *tcp, unsigned idx) {
    WizSocketState *st = &tcp->s[idx];
    uint8_t sock = WIZ_SOCKET_OF(idx);

    wiz_process_pending_active_close(tcp, idx);
    if(st->pendingActiveClose)
        return;

    uint8_t sr = getSn_SR(sock);

    if(sr == SOCK_ESTABLISHED) {
        wiz_signal_active_open(tcp, idx);

        uint16_t rxSize = getSn_RX_RSR(sock);
        if(rxSize > 0u) {
            if(rxSize > sizeof(tcp->rxBuffer))
                rxSize = sizeof(tcp->rxBuffer);

            int32_t received = recv(sock, tcp->rxBuffer, rxSize);
            WIZ_DBG("rx s%u rsr=%u recv=%ld announced=%d cb=%d\r\n",
                    sock, rxSize, (long)received, st->activeAnnounced,
                    tcp->callback != NULL);

            /* Admission control, before the bytes reach the server.
             *
             * Once an OPN is handed over, the RSA work is committed and cannot
             * be taken back, so the decision has to happen here. Refusing at
             * this point costs the client a TCP reset it can retry; letting it
             * through costs every client in flight five seconds and leaves a
             * session occupying a slot for ten minutes. */
            if(received > 0 && WIZ_IS_OPN(tcp->rxBuffer, (size_t)received) &&
               wiz_opn_needs_asymmetric(tcp->rxBuffer, (size_t)received) &&
               !st->handshaking && !st->channelOpened &&
               WIZ_HANDSHAKE_BUDGET_MS > 0u) {
                uint32_t now = to_ms_since_boot(get_absolute_time());
                uint32_t waited = now - st->connectedAtMs;
                uint32_t wouldFinishAt = waited + WIZ_HANDSHAKE_COST_MS;

                if(wouldFinishAt > WIZ_HANDSHAKE_BUDGET_MS) {
                    g_wizHandshakeRefused++;
                    printf("[el] handshake refused s%u "
                           "(waited %lums + %ums > %ums, refused=%lu) "
                           "%u.%u.%u.%u:%u\n",
                           sock, (unsigned long)waited, WIZ_HANDSHAKE_COST_MS,
                           WIZ_HANDSHAKE_BUDGET_MS,
                           (unsigned long)g_wizHandshakeRefused,
                           st->peerIp[0], st->peerIp[1],
                           st->peerIp[2], st->peerIp[3], st->peerPort);
                    wiz_queue_active_close(tcp, idx);
                    return;
                }

                st->handshaking = true;
            }

            if(received > 0 && st->activeAnnounced && tcp->callback) {
                UA_ByteString msg;
                msg.length = (size_t)received;
                msg.data = tcp->rxBuffer;
                tcp->callback(&tcp->cm, WIZ_ACTIVE_ID(idx),
                              tcp->application, &st->activeContext,
                              UA_CONNECTIONSTATE_ESTABLISHED,
                              &UA_KEYVALUEMAP_NULL, msg);
            } else if(received < 0) {
                wiz_queue_active_close(tcp, idx);
            }
        }
        return;
    }

    if(sr == SOCK_CLOSE_WAIT) {
        disconnect(sock);
        wiz_queue_active_close(tcp, idx);
        return;
    }

    if(st->activeAnnounced &&
       (sr == SOCK_CLOSED || sr == SOCK_INIT || sr == SOCK_LISTEN)) {
        wiz_queue_active_close(tcp, idx);
        return;
    }

    wiz_open_listen_socket(tcp, idx);
}

#if WIZ_EL_DEBUG
static uint32_t g_wizDbgLast = 0;
static void wiz_dbg_dump(WizTcpConnectionManager *tcp) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if(now - g_wizDbgLast < 2000u)
        return;
    g_wizDbgLast = now;
    printf("[el] n=%u", g_wizSocketCount);
    for(unsigned i = 0; i < g_wizSocketCount; i++) {
        uint8_t sk = WIZ_SOCKET_OF(i);
        printf("  s%u:sr=0x%02X rsr=%u ann=%d", sk, getSn_SR(sk),
               getSn_RX_RSR(sk), tcp->s[i].activeAnnounced);
    }
    printf("  listenAnn=%d\r\n", tcp->listenAnnounced);
}
#endif

static void wiz_cm_poll(WizTcpConnectionManager *tcp) {
    if(tcp->cm.eventSource.state != UA_EVENTSOURCESTATE_STARTED)
        return;

#if WIZ_EL_DEBUG
    wiz_dbg_dump(tcp);
#endif

    wiz_process_pending_listen_close(tcp);
    if(tcp->pendingListenClose)
        return;

    for(unsigned i = 0; i < g_wizSocketCount; i++)
        wiz_cm_poll_socket(tcp, i);
}

static UA_StatusCode wiz_cm_start(UA_EventSource *es) {
    es->state = UA_EVENTSOURCESTATE_STARTED;
    return UA_STATUSCODE_GOOD;
}

static void wiz_cm_stop(UA_EventSource *es) {
    WizTcpConnectionManager *tcp = (WizTcpConnectionManager *)es;

    for(unsigned i = 0; i < g_wizSocketCount; i++) {
        tcp->s[i].pendingActiveClose = tcp->s[i].activeAnnounced;
        wiz_process_pending_active_close(tcp, i);
    }
    tcp->pendingListenClose = tcp->listenAnnounced;
    wiz_process_pending_listen_close(tcp);
    es->state = UA_EVENTSOURCESTATE_STOPPED;
}

static UA_StatusCode wiz_cm_free(UA_EventSource *es) {
    if(!es)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    if(es->state == UA_EVENTSOURCESTATE_STARTED)
        wiz_cm_stop(es);

    UA_String_clear(&es->name);
    UA_KeyValueMap_clear(&es->params);
    UA_free(es);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
wiz_cm_open_connection(UA_ConnectionManager *cm, const UA_KeyValueMap *params,
                         void *application, void *context,
                         UA_ConnectionManager_connectionCallback callback) {
    WizTcpConnectionManager *tcp = (WizTcpConnectionManager *)cm;
    const UA_UInt16 *port = (const UA_UInt16 *)UA_KeyValueMap_getScalar(
        params, UA_QUALIFIEDNAME(0, "port"), &UA_TYPES[UA_TYPES_UINT16]);
    const UA_Boolean *listenFlag = (const UA_Boolean *)UA_KeyValueMap_getScalar(
        params, UA_QUALIFIEDNAME(0, "listen"), &UA_TYPES[UA_TYPES_BOOLEAN]);

    if(!port || !listenFlag || !*listenFlag)
        return UA_STATUSCODE_BADNOTIMPLEMENTED;

    tcp->port = *port;
    tcp->application = application;
    tcp->listenContext = context;
    tcp->callback = callback;
    tcp->listenAnnounced = false;
    tcp->pendingListenClose = false;

    for(unsigned i = 0; i < g_wizSocketCount; i++) {
        tcp->s[i].activeContext = NULL;
        tcp->s[i].activeAnnounced = false;
        tcp->s[i].pendingActiveClose = false;
        tcp->s[i].handshaking = false;
        tcp->s[i].channelOpened = false;
        close(WIZ_SOCKET_OF(i));
    }
    for(unsigned i = 0; i < g_wizSocketCount; i++)
        wiz_open_listen_socket(tcp, i);

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
wiz_cm_send(UA_ConnectionManager *cm, uintptr_t connectionId,
              const UA_KeyValueMap *params, UA_ByteString *buf) {
    (void)params;
    WizTcpConnectionManager *tcp = (WizTcpConnectionManager *)cm;
    UA_StatusCode result = UA_STATUSCODE_GOOD;

    int idx = wiz_index_of_active_id(connectionId);
    if(idx < 0 || !buf || !buf->data || !tcp->s[idx].activeAnnounced) {
        WIZ_DBG("send REJECT id=%lu idx=%d buf=%d announced=%d\r\n",
                (unsigned long)connectionId, idx, buf && buf->data,
                (idx >= 0) ? tcp->s[idx].activeAnnounced : -1);
        result = UA_STATUSCODE_BADCONNECTIONCLOSED;
        goto out;
    }

    if(wiz_send_all(WIZ_SOCKET_OF(idx), buf->data, (uint16_t)buf->length) != 0) {
        result = UA_STATUSCODE_BADCONNECTIONCLOSED;
        wiz_queue_active_close(tcp, (unsigned)idx);
    }

    /* The handshake slot is held from the OPN request until its response goes
     * out, and the first send on this socket after that request is it. Clearing
     * on any send is deliberate: if the server answers with an error instead,
     * the slot still has to come back. */
    if(tcp->s[idx].handshaking)
        tcp->s[idx].channelOpened = true;
    tcp->s[idx].handshaking = false;

out:
    if(buf)
        UA_ByteString_clear(buf);
    return result;
}

static UA_StatusCode wiz_cm_close(UA_ConnectionManager *cm,
                                    uintptr_t connectionId) {
    WizTcpConnectionManager *tcp = (WizTcpConnectionManager *)cm;

    int idx = wiz_index_of_active_id(connectionId);
    if(idx >= 0) {
        disconnect(WIZ_SOCKET_OF(idx));
        wiz_queue_active_close(tcp, (unsigned)idx);
        return UA_STATUSCODE_GOOD;
    }

    if(connectionId == WIZ_LISTEN_CONNECTION_ID) {
        tcp->pendingListenClose = true;
        return UA_STATUSCODE_GOOD;
    }

    return UA_STATUSCODE_BADNOTFOUND;
}

static UA_StatusCode
wiz_cm_alloc_buffer(UA_ConnectionManager *cm, uintptr_t connectionId,
                      UA_ByteString *buf, size_t bufSize) {
    (void)cm;
    (void)connectionId;
    return UA_ByteString_allocBuffer(buf, bufSize);
}

static void wiz_cm_free_buffer(UA_ConnectionManager *cm,
                                 uintptr_t connectionId,
                                 UA_ByteString *buf) {
    (void)cm;
    (void)connectionId;
    UA_ByteString_clear(buf);
}

UA_ConnectionManager *
UA_ConnectionManager_new_LWIP_TCP(const UA_String eventSourceName) {
    WizTcpConnectionManager *tcp =
        (WizTcpConnectionManager *)UA_calloc(1, sizeof(WizTcpConnectionManager));
    if(!tcp)
        return NULL;

    UA_ConnectionManager *cm = &tcp->cm;
    UA_String_copy(&eventSourceName, &cm->eventSource.name);
    cm->eventSource.eventSourceType = UA_EVENTSOURCETYPE_CONNECTIONMANAGER;
    cm->eventSource.state = UA_EVENTSOURCESTATE_FRESH;
    cm->eventSource.start = wiz_cm_start;
    cm->eventSource.stop = wiz_cm_stop;
    cm->eventSource.free = wiz_cm_free;
    cm->protocol = UA_STRING("tcp");
    cm->openConnection = wiz_cm_open_connection;
    cm->sendWithConnection = wiz_cm_send;
    cm->closeConnection = wiz_cm_close;
    cm->allocNetworkBuffer = wiz_cm_alloc_buffer;
    cm->freeNetworkBuffer = wiz_cm_free_buffer;
    tcp->port = WIZ_OPCUA_TCP_PORT;

    return cm;
}

UA_ConnectionManager *
UA_ConnectionManager_new_LWIP_UDP(const UA_String eventSourceName) {
    (void)eventSourceName;
    return NULL;
}
