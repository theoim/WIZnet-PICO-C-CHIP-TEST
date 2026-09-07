# current_state.md — 현재 구현이 무엇을 하고 있는가

> **이 문서는 사실 기술이다.** "무엇을 해야 하는가"는 여기에 쓰지 않는다.
> 사양(결정 사항)은 별도 문서에 둔다. 이 문서를 사양으로 승격시키면
> 현재 버그가 사양으로 굳는다.

조사일: 2026-09-03 · 브랜치 `main` · 커밋 `7ff1f56`

> **참고.** 이 문서가 인용하는 `TASK_BRIEF.md` 와 `product_direction.md` 는
> 사내 기획 문서라 이 저장소에 포함하지 않는다. 구현에 반영된 결정은
> 소스 주석과 `docs/` 의 측정 문서에 옮겨 두었다.

---

## 1. 브랜치 상황

`opcua_node`는 원래 `board/rp2354b-w6300-cam` 브랜치에만 있었다.
**2026-09-03에 main으로 가져왔다** (`git checkout board/rp2354b-w6300-cam -- examples/opcua_node`).

| 브랜치 | BOARD_NAME | `opcua_node` | `opcua_usb_stdio` |
|---|---|---|---|
| `main` (현재) | `W6300_EVB_PICO2` | **있음 (29파일)** | 있음 |
| `board/rp2354b-w6300-cam` | `W6300_RP2354B_CAM` | 있음 (원본) | 있음 |
| `demo/exhibition` | — | 없음 | — |

`port/open62541`은 두 브랜치가 **완전히 동일**하다. cam 브랜치의 port 차이는
카메라 보드 전용(`w6300_rp2354b_cam_pins.h`, `board_list.h`, `wizchip_spi.h`)이라
`opcua_node`가 필요로 하지 않는다.

**두 예제 모두 `examples/CMakeLists.txt`에서 활성화되어 있고 함께 빌드된다.**

---

## 2. 빌드 시스템

- 루트 `CMakeLists.txt:13`이 `set(BOARD_NAME ...)`로 보드를 **고정**한다.
  캐시 변수가 아니라 평범한 `set`이라 `-DBOARD_NAME=`으로 덮을 수 없다.
- open62541은 `port/open62541/CMakeLists.txt`가 **amalgamation을 자동 생성**한다.
  `libraries/open62541` 소스를 호스트 컴파일러로 빌드해서 단일 `.c`/`.h`를 만든다.
- 생성 후 `initNS0_dataSources → initNS0` **문자열 패치를 강제 적용**하고,
  패치가 실패하면 `FATAL_ERROR`로 중단한다.
- `OPEN62541_WIZNET` 정적 라이브러리(`port/open62541/CMakeLists.txt:81`)는
  `if(NOT TARGET ...)` 가드로 **한 번만** 만들어져 두 예제가 공유한다.
- 트랜스포트는 lwIP가 아니라 **WIZnet ioLibrary 소켓 위에 `UA_ARCHITECTURE_LWIP`
  쉼(shim)을 씌운 커스텀 이벤트루프**다. `lwip/sockets.h`는 스텁.

amalgamation 플래그 전체는 `baseline.md` 6장에 있다. 요약하면
`NAMESPACE_ZERO=MINIMAL`, Subscriptions/MethodCalls/NodeManagement ON,
PubSub/Discovery/Historizing OFF, `MULTITHREADING=0`,
**`UA_ENABLE_ENCRYPTION` 플래그 없음(암호화 비활성)**.

---

## 3. 트랜스포트 — 동시 연결 1개

`port/open62541/open62541_wiznet_eventloop.c`

```c
#define WIZ_UA_SOCKET            0u      // :11  하드웨어 소켓 0번 하나만
#define WIZ_LISTEN_CONNECTION_ID 1u      // :13
#define WIZ_ACTIVE_CONNECTION_ID 2u      // :14
#define WIZ_RX_BUFFER_SIZE       8192u   // :15
```

listen 1 + active 1 구조이며, 두 역할이 **같은 하드웨어 소켓 0번**을 쓴다.
따라서 **동시에 유지되는 TCP 연결은 1개다.**

W6300은 하드웨어 소켓이 8개이므로 하드웨어 여유는 있다. 다만 소켓마다
SecureChannel 버퍼가 붙으면 RAM이 선형 증가한다 (미측정).

---

## 4. 주소공간

### 4.1 `opcua_node` — 동적 생성

네임스페이스: `urn:wiznet:opcua-node` (`inc/opcua_node_map.h`)

```
Objects
└── <DeviceName>                   ns=1;i=1000   (OPCUA_ID_ROOT)
    ├── Device                     ns=1;i=1001   (OPCUA_ID_DEVICE)
    │   └── 진단 변수 15종         ns=1;s="Diag.*"
    │       DeviceName / IPAddress / FwVersion / Uptime_s / FreeHeap /
    │       RebootCount / RebootReason / LinkStatus / RawFrame /
    │       UsbFrameCount / UsbErrorCount / UartFrameCount / UartErrorCount /
    │       ModbusPollCount / ModbusTimeoutCount
    └── Channels                   ns=1;i=1002   (OPCUA_ID_CHANNELS)
        └── <channel name>         ns=1;s="Channels/<name>"   (DataSource)
            ├── EngineeringUnits   ns=1;i=5000 + idx*4 + 0
            └── EURange            ns=1;i=5000 + idx*4 + 1
```

| 상수 | 값 |
|---|---|
| `OPCUA_ID_DIAG_BASE` | 3000 |
| `OPCUA_ID_PROP_BASE` | 5000 |
| `OPCUA_DIAG_CTX_BASE` | 0x1000 |
| `OPCUA_MAX_CHANNELS` | **32** |

읽기 콜백의 `node_context` 규약:
- 채널: 채널 인덱스 (0 .. count−1)
- 진단: `OPCUA_DIAG_CTX_BASE + kind`

채널은 `OPCUA_MAX_CHANNELS < 0x1000`이라 두 영역이 겹치지 않는다.

### 4.2 `opcua_usb_stdio` — 하드코딩 (레퍼런스)

```
Objects
└── Sensor_Node                    ns=1;i=1000
    ├── Device                     ns=1;i=1001
    │   └── Device.Name / .IP / .FwVer / .Uptime
    └── USB_Stdio                  ns=1;i=2000
        └── Input.RawFrame / .Ch1 / .Ch2 / .Ch3 /
            .FrameCount / .ParseErrorCount
```

3채널 고정. `TASK_BRIEF.md`가 회귀 기준으로 보존하라고 지정한 예제다.

### 4.3 공통 — pull 모델

값 노출은 **DataSource 읽기 콜백(pull) 모델**이다.
`UA_Server_addDataSourceVariableNode` + 읽기 콜백이 `data_table` 스냅샷을 읽는다.
**노드에 값을 push하지 않는다.**

---

## 5. 고정값과 설정값

### 5.1 `opcua_node`

| 항목 | 값 | 위치 |
|---|---|---|
| Application URI | `urn:WIZnet:opcua-node` | `inc/opcua_server.h:11` |
| Product URI | `urn:WIZnet:RP2350` | `:12` |
| TCP 포트 | 4840 | `inc/wiznet_network.h:9` |
| 최대 채널 | 32 | `inc/opcua_settings.h:29` |
| SecurityPolicy | None | amalgamation에 암호화 없음 |

엔드포인트 URL은 `s_endpoint[48]` (`.bss`)에 **런타임 생성**된다.
`opcua_usb_stdio`처럼 문자열 상수가 아니다.

네트워크·채널 정의는 `opcua_settings_t`(플래시, 2,816 B)에서 읽는다.
`magic` + `schema_version` + `channel_count` + `channel_def_t[32]` 구조.

### 5.2 `opcua_usb_stdio` (참고)

Endpoint URL이 `opc.tcp://192.168.11.2:4840`로 **하드코딩**되어 있다
(`inc/opcua_server.h:6`). Application URI는 `urn:WIZnet:EVB-PICO2:OpcUaUsbStdio`.

---

## 6. 실행 구조

`src/main.c` — 단일 코어 협력형 폴링 루프. 듀얼코어 미사용.

```c
wiznet_network_init(&g_settings);
opcua_server_init(&g_settings);
while(1) {
    usb_stdio_link_poll();
    wiznet_network_poll();
    opcua_server_poll();
    sleep_ms(1);
}
```

`UA_MULTITHREADING=0`과 일치한다. **어느 한 단계가 오래 걸리면 나머지가 전부 멈춘다.**
RSA 개인키 연산 같은 긴 작업이 들어오면 이 루프 전체가 정지한다 (Phase B 고려사항).

W6300 칩 초기화는 `wizchip_initialize()`의 무한 PHY 대기를 우회한 논블로킹 버전이다.
LAN 케이블 없이도 USB CLI가 살아있게 하려는 의도.

---

## 7. 데이터 소스

현재 인입 경로는 **USB CDC 하나뿐**이다.

- `usb_stdio_link.c` — `$DATA` 프레임 파서 + CLI
  (`GET` / `NET` / `OPCUA` / `CLEAR` / `HELP`, `?`와 `DATA:` 무접두 변형도 수용)
- `data_table.c` — `s_values` (`channel_value_t` × 32, 3,840 B)

`TASK_BRIEF.md`가 목표로 하는 `MODBUS_RTU` / `UART_RAW` 인입은 **미구현**이다.
진단 enum에 `MODBUS_POLLS` / `MODBUS_TIMEOUTS` 자리만 있고 값은 채워지지 않는다.

`opcua_node`는 TinyUSB **vendor class도 활성화**되어 있다
(`_vendord_itf` 588 B, `_vendord_epbuf` 128 B). 용도는 확인하지 않았다.

---

## 8. 문서 관계

| 문서 | 성격 | 상태 |
|---|---|---|
| `TASK_BRIEF.md` | **지시서 + 사양** | Phase A 태스크 A-0~A-6 정의. 사실상 사양 하네스. 사내 문서로 이 저장소에는 미포함 |
| `docs/baseline.md` | 측정 결과 | main 기준으로 갱신됨. 두 예제 모두 측정 |
| `docs/current_state.md` | 현재 상태 | 이 문서 |
| `docs/memory_budget.md` | 측정 방법 + 목표 | 정적 예측치는 실측과 **일치 확인됨**. heap 표는 여전히 TODO |
| `docs/node_map.md` | 노드맵 설계 | 실제 구현과의 일치 여부 **미확인** |

`TASK_BRIEF.md` §0을 2026-09-03에 재검증했다. **서술은 전부 사실과 일치했다.**
단 두 가지 예외:

| §0 서술 | 실제 |
|---|---|
| "루트 `CMakeLists.txt:13` `set(BOARD_NAME W6300_EVB_PICO2)`" | main에서는 정확. cam 브랜치는 `:14 W6300_RP2354B_CAM` |
| A-0 "주석 해제가 첫 커밋이다" | 주석 해제만으로는 빌드되지 않았다. 9장 참조 |

---

## 9. 2026-09-03에 고친 것

`opcua_node`는 **이 수정 전까지 빌드된 적이 없다.**

| 파일 | 내용 |
|---|---|
| `inc/opcua_node_map.h:13` | 주석 안의 `Usb*/`가 블록 주석을 조기 종료 → 컴파일 오류 51개. `Usb / Uart / Modbus`로 수정 |
| `examples/opcua_node/CMakeLists.txt` | `if(NOT TARGET OPEN62541_WIZNET)` 가드 |
| `examples/opcua_usb_stdio/CMakeLists.txt` | 동일 가드 |
| `.gitignore` | `build*/` 추가 |

빌드 함정 5종은 `baseline.md` 9장에 있다.

---

## 10. 확인하지 않은 것

- `docs/node_map.md` 내용과 실제 구현의 일치 여부
- `opcua_settings.c`의 플래시 섹터 위치와 `PICO_FLASH_SIZE_BYTES` 실제값
- `UA_TYPES`(27.3 KB)가 `.rodata`로 갈 수 있는지
- TinyUSB vendor class를 무엇에 쓰는지
- 진단 노드 15종이 실제로 값을 채우는지 (`MODBUS_*`는 인입 미구현)
- **실기 동작 일체** — 굽지 않았다. heap peak, stack high-water 모두 미측정
