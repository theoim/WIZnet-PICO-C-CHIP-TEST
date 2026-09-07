#ifndef WIZNET_EVENTLOOP_CONFIG_H
#define WIZNET_EVENTLOOP_CONFIG_H

#include <stdint.h>

/*
 * WIZnet 이벤트루프 런타임 설정.
 *
 * OPEN62541_WIZNET 은 모든 OPC UA 예제가 공유하는 하나의 정적 라이브러리다.
 * 따라서 예제별 compile definition 으로는 값을 나눌 수 없다 — 먼저 처리된
 * 예제의 값이 라이브러리 전체에 적용된다. 그래서 런타임 설정으로 둔다.
 *
 * 기본값은 소켓 1개이므로, 아무것도 호출하지 않는 예제는 기존 동작 그대로다.
 * (opcua_usb_stdio 는 회귀 기준이라 손대지 않는다 — TASK_BRIEF 참조)
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
 *       (docs/product_direction.md D-5)
 */
void UA_EventLoop_LWIP_setSocketCount(uint8_t count);

/* 현재 설정값을 돌려준다. 진단·로그용. */
uint8_t UA_EventLoop_LWIP_getSocketCount(void);

#ifdef __cplusplus
}
#endif

#endif /* WIZNET_EVENTLOOP_CONFIG_H */
