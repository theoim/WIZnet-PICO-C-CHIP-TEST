# DEVLOG — opcua_node

날짜별 개발 로그. 최신 항목이 위. 디버깅 시 "원래 코드가 뭐였고 왜 이렇게 바꿨는지" 참조용.

> **참고.** 아래 항목들이 인용하는 `TASK_BRIEF.md` 와 `docs/product_direction.md` 는
> 사내 기획 문서라 이 저장소에 포함하지 않는다. 구현에 필요한 결정 내용
> (소켓 예산, open62541 리소스 상한, 버퍼 크기 제약)은 해당 소스 주석과
> `docs/` 의 측정 문서에 그대로 옮겨 두었으므로 코드만 읽어도 따라올 수 있다.

---

## 2026-07-13 — 최초 구현 (TASK_BRIEF.md A-0 ~ A-1)

### 출발점

`examples/opcua_usb_stdio/`를 통째로 복사해서 시작 (`opcua_node/`). 원본은 절대 수정 안 함 —
회귀 비교 기준으로 그대로 둠. 원본 vs 이 디렉터리 diff 뜨고 싶으면:

```
diff -ru examples/opcua_usb_stdio examples/opcua_node
```

지시서: `TASK_BRIEF.md` (이 디렉터리 루트). 0장에 원본 코드 실측 사실 정리돼 있음 — 파일 라인수,
open62541 amalgamation 빌드 플래그, 소켓 배치, DataSource pull 모델 등. 못 찾겠으면 거기 먼저 봐라.

### 무엇을 왜 바꿨나

**문제의식**: 원본은 3채널 고정(`ch1/ch2/ch3` float), 노드맵이 `opcua_node_map.h`에 완전
하드코딩(`#define OPCUA_NODE_INPUT_CH1 "Input.Ch1"` 식). 채널 추가하려면 재컴파일 필요.
목표는 "컴파일 없이 채널 정의 → 재부팅 → 새 주소공간"이 되는 구조.

**신규 파일** (원본에 없던 것):
- `inc/opcua_settings.h` + `src/opcua_settings.c` — flash 영속 설정. 채널 정의 32개 배열,
  network/auth 필드, magic+schema_version+**CRC32** 무결성. 이게 이번 구현의 핵심 신규 모듈.
- `docs/node_map.md`, `docs/memory_budget.md` — 스키마 문서 + 메모리 측정 표(TODO, 실측 안 됨).

**수정한 파일**:

| 파일 | 원본 | 바뀐 것 | 왜 |
|---|---|---|---|
| `inc/opcua_node_map.h` | 채널3개+디바이스4개 `#define` 문자열 하드코딩 | NodeId 할당 규칙 + `OpcUaDiagKind` enum으로 재정의 (매크로가 아니라 콜백 컨텍스트 태그) | 채널 수가 런타임에 정해지므로 컴파일타임 문자열 노드ID 방식 폐기 |
| `inc/data_table.h`/`.c` | `DataTable` 구조체 하나, ch1/ch2/ch3 필드 고정 | `channel_value_t s_values[32]` 배열 + `diag_counters_t`. `data_table_set_channels()` → `data_table_set_raw/bool/string/status()` | 채널 개수 가변화 + **StatusCode 품질** 개념 추가(Modbus Bad_CommunicationError 노출용, A-2 선행 준비) |
| `src/opcua_server.c` | `add_address_space()`가 Device 4노드 + USB_Stdio 6노드를 함수 호출 나열로 직접 생성 | 같은 함수가 `s_cfg->channels[]`를 for문으로 순회하며 생성. 진단노드 15종으로 확장. endpoint URL도 `snprintf`로 런타임 생성(원본은 `#define OPCUA_ENDPOINT_URL "opc.tcp://192.168.11.2:4840"` 고정 문자열) | 동일 함수를 정적 나열 → 동적 순회로 바꾼 것. **원본 구조(add_object_node/add_data_variable 패턴)는 그대로 재사용**, 감싸는 반복문만 추가 |
| `inc/opcua_server.h` | `#define OPCUA_ENDPOINT_URL` 등 IP 하드코딩 매크로 | 매크로 제거, `opcua_server_init(const opcua_settings_t *cfg)`로 IP 인자 전달 | IP가 설정값이 됨 |
| `src/usb_stdio_link.c` | `data_table_set_channels(ch1,ch2,ch3,raw)` 직접 호출 | `data_table_feed_usb_floats(v, 3, raw)` — USB 소스 채널에 순서대로 먹임 | 데모 명령(`$DATA:`, GET/NET/OPCUA/CLEAR/HELP)은 **그대로 동작해야 함**(회귀 금지 조항). 파싱 로직 자체는 안 건드림, 결과를 어디 저장하는지만 바꿈 |
| `src/wiznet_network.c` | `wiz_NetInfo s_net_info` 정적 초기화(IP 192.168.11.2 고정) | `wiznet_network_init(cfg)`가 시작 시 `cfg->ip/gw/sn/dns`를 `s_net_info`에 memcpy | DHCP는 아직 미구현(A-4 예정) — **현재도 static 방식, 값만 설정에서 옴** |
| `src/main.c` | `data_table_init()`, `usb_stdio_link_init()`, `wiznet_network_init()`, `opcua_server_init()` 전부 무인자 | 부팅 시 `opcua_settings_load(&g_settings)` 먼저 호출, 나머지 전부 `&g_settings` 전달 | 설정 로드가 모든 것의 선행 조건이 됨 |
| `examples/CMakeLists.txt` | `opcua_usb_stdio`/`opcua_node` 둘 다 주석 처리 | `opcua_node`만 주석 해제 | 이 예제 빌드 활성화. `opcua_usb_stdio`는 여전히 비활성 — 필요하면 직접 주석 해제해서 원본과 비교 빌드 가능 |

### 검증 못 한 것 (하드웨어 없어서)

- **컴파일 자체를 안 해봄.** 이 세션 환경에 Pico SDK 빌드 도구 없음. 첫 디버깅은 100%
  `cmake --build` 에러 로그부터 시작될 것 — 아마 헤더 include 순서나 open62541 타입 이름 오타.
- `opcua_server.c`의 `free_heap()` 함수가 쓰는 `__StackLimit`/`__bss_end__` 심볼 — Pico 표준
  링커 심볼 맞는지 실물 링크에서 확인 안 됨. 링크 에러 나면 여기부터 봐라.
- `EURange`를 `AnalogItemType` 안 쓰고 `Double[2]` Property로 대체함(MINIMAL NS0에 AnalogItem
  없어서). UAExpert에서 이게 정상적으로 안 보이면 `docs/node_map.md` "EURange 결정" 항목 참고.
- 32채널 실측 메모리 — `docs/memory_budget.md` 표가 전부 TODO. 채널 늘리다 힙 터지면 여기부터.

### 구조적으로 원본과 동일하게 유지한 것 (건드리면 안 되는 지점)

- open62541 DataSource **pull 콜백** 모델 그대로. `opcua_read_value()`가 여전히 유일한 read 진입점.
  값을 노드에 push하는 방식으로 바꾸지 마라 — 단일코어 협력루프라 pull이 맞음.
- `main.c`의 단일 while 루프 구조 (`usb_stdio_link_poll → wiznet_network_poll →
  opcua_server_poll → sleep_ms(1)`) 그대로. Core1 안 씀.
- W6300 논블로킹 init (`wiznet_chip_initialize_nonblocking`, PHY 대기 스킵) 그대로 — LAN 케이블
  없어도 USB CLI 살아있게 하는 원본 의도 유지.

### 다음 세션이 이어서 할 것

TASK_BRIEF.md A-2(Modbus RTU) ~ A-6. settings 구조체에 이미 modbus_baud/parity/uart_start_token
등 필드 예약해뒀으니 A-2/A-3은 그거 채워 넣으면 됨. A-4(웹 대시보드)는 `wiz_claw_webserver.c`
(`examples/wiz_claw_spi_host/`) 이식 대상 — 단 그 파일 시크릿 하드코딩/인증없음 그대로 베끼지
말 것, TASK_BRIEF.md 0.4절 "주의 1/2/3" 참고.
