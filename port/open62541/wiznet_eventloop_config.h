#ifndef WIZNET_EVENTLOOP_CONFIG_H
#define WIZNET_EVENTLOOP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/*
 * WIZnet 이벤트루프 런타임 설정.
 *
 * OPEN62541_WIZNET 은 모든 OPC UA 예제가 공유하는 하나의 정적 라이브러리다.
 * 따라서 예제별 compile definition 으로는 값을 나눌 수 없다 — 먼저 처리된
 * 예제의 값이 라이브러리 전체에 적용된다. 그래서 런타임 설정으로 둔다.
 *
 * 기본값은 소켓 1개이므로, 아무것도 호출하지 않는 예제는 기존 동작 그대로다.
 * (opcua_usb_stdio 는 회귀 비교 기준이라 손대지 않는다)
 */

#ifdef __cplusplus
extern "C" {
#endif

/* OPC UA 가 동시에 LISTEN 시킬 하드웨어 소켓 개수를 정한다.
 *
 * 반드시 서버를 시작하기 전에 호출해야 한다. 서버가 이미 listen 을 연 뒤에
 * 부르면 이번 실행에는 반영되지 않는다.
 *
 * count 는 1 이상, WIZ_UA_SOCKET_MAX(8) 이하로 클램프된다.
 * 소켓 번호는 0 부터 count-1 까지를 점유한다.
 *
 * 주의: 여기서 정한 개수만큼 소켓 버퍼가 배분되어 있어야 한다.
 *       버퍼 배분은 애플리케이션이 wizchip 초기화 때 직접 넘긴다.
 *       예: examples/opcua_node/src/wiznet_network.c 의 memsize
 */
void UA_EventLoop_LWIP_setSocketCount(uint8_t count);

/* 현재 설정값을 돌려준다. 진단·로그용. */
uint8_t UA_EventLoop_LWIP_getSocketCount(void);

/* Transport counters for diagnostics and soak tests. A socket that never
 * comes back (a leak) shows as socketsBusy staying up with no client
 * connected; the three counters are cumulative since boot. */
typedef struct {
    uint8_t  socketsBusy;       /* hardware sockets neither LISTEN nor CLOSED */
    uint32_t handshakeRefused;  /* OPNs answered with Bad_TcpServerTooBusy    */
    uint32_t closeCut;          /* closes forced after the closing-state cap  */
    uint32_t chipDropped;       /* connections the chip's keepalive gave up on */
} UA_EventLoop_LWIP_Stats;

void UA_EventLoop_LWIP_getStats(UA_EventLoop_LWIP_Stats *out);

/* ── 시각 동기화 ───────────────────────────────────────────────────
 * 이 포팅 계층은 시계를 두 개로 분리해서 제공한다.
 *
 *   UA_DateTime_nowMonotonic()  부팅 기준 단조 시간. 오프셋 미포함.
 *                               open62541 의 타이머가 쓰는 시계다.
 *   UA_DateTime_now()           벽시계. 동기화 전에는 1970년이다.
 *
 * 아래 두 함수는 그 벽시계만 조정한다. monotonic 은 바뀌지 않으므로
 * 이미 걸려 있는 구독·세션·재전송 타이머가 깨지지 않는다.
 *
 * SNTP 등 시각원을 붙일 때 반드시 wiz_clock_set_utc() 를 쓰고,
 * pico_monotonic 쪽에 오프셋을 더하지 않는다. */

/* 동기화된 현재 UTC 를 알려준다.
 * 단위는 UA_DateTime 과 같다 — 1601-01-01 기준 100ns 틱. 이 헤더는
 * open62541.h 보다 먼저 포함될 수 있어 int64_t 로 받는다(같은 타입이다). */
void wiz_clock_set_utc(int64_t utc_now);

/* 한 번이라도 동기화됐으면 true. 애플리케이션은 이 값을 보고
 * sourceTimestamp 를 붙일지 결정한다. */
bool wiz_clock_is_synced(void);

#ifdef __cplusplus
}
#endif

#endif /* WIZNET_EVENTLOOP_CONFIG_H */
