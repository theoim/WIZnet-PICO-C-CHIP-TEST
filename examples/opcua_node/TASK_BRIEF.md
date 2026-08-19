# TASK_BRIEF: WIZnet OPC UA 노드 — Phase A 제품화

> 이 문서는 CLI 코딩 에이전트(Opus/Sonnet)용 작업지시서다. 이 디렉터리(`examples/opcua_node/`)가 작업 대상이다.
> 이 예제는 `examples/opcua_usb_stdio/`의 사본에서 출발했다. **원본 `opcua_usb_stdio/`는 절대 수정하지 마라** — 회귀 기준(레퍼런스)으로 보존한다.
> 아래 0장은 2026-07-13에 로컬 코드를 실측 검증한 사실이다. 추측이 아니므로 재확인 없이 전제로 사용해도 된다.

---

## 0. 검증된 현재 상태 (사실 — 변경 금지 아님, 출발점)

### 0.1 파일 구성 (이 디렉터리)
```
opcua_node/
├── CMakeLists.txt            # TARGET_NAME opcua_node, OPCUA_ENABLE=1, OPCUA_TCP_PROBE_ENABLE=0
├── src/
│   ├── main.c                # 27줄. 협력형 단일 루프: usb_stdio_link_poll → wiznet_network_poll → opcua_server_poll → sleep_ms(1)
│   ├── opcua_server.c        # 503줄. open62541 래핑, 주소공간 하드코딩
│   ├── opcua_tcp_probe.c     # HEL/ACK 전용 프로브 (OPCUA_ENABLE=0일 때만, 현재 비활성)
│   ├── usb_stdio_link.c      # $DATA 프레임 파서 + CLI (GET/NET/OPCUA/CLEAR/HELP, "?"와 "DATA:" 무접두 변형도 수용)
│   ├── data_table.c          # 3채널 고정 테이블 (ch1/ch2/ch3 float, raw[…], frame_count, parse_error_count)
│   └── wiznet_network.c      # 정적 IP 192.168.11.2/24 gw .1, W6300 논블로킹 init(PHY 대기 스킵), 1s 링크 폴
├── inc/                      # 대응 헤더 6개. opcua_node_map.h = 노드맵 하드코딩
└── README_kor.md / README_eng.md / images/
```

### 0.2 빌드 시스템 (중요 — 지시서 원안에 없던 사실)
- 보드: 루트 `CMakeLists.txt:13` `set(BOARD_NAME W6300_EVB_PICO2)` → RP2350 + W6300(QSPI QUAD). `-D_WIZCHIP_=W6300`.
- open62541은 `port/open62541/CMakeLists.txt`가 **amalgamation을 자동 생성**한다 (`libraries/open62541` 소스에서). 현재 생성 플래그:
  `UA_ARCHITECTURE=none, UA_NAMESPACE_ZERO=MINIMAL, UA_ENABLE_SUBSCRIPTIONS=ON, UA_ENABLE_METHODCALLS=ON, UA_ENABLE_NODEMANAGEMENT=ON, UA_MULTITHREADING=0, PUBSUB/DISCOVERY/HISTORIZING=OFF`
  생성 후 `initNS0_dataSources → initNS0` 문자열 패치를 강제 적용 (패치 실패 시 FATAL). 이 플래그 목록이 `docs/memory_budget.md`의 기준선이다.
- 트랜스포트: `port/open62541/open62541_wiznet_eventloop.c` (약 21KB) — lwip이 아니라 **WIZnet ioLibrary 소켓 위에 UA_ARCHITECTURE_LWIP 쉼(shim)을 씌운 커스텀 이벤트루프**. `lwip/sockets.h`는 스텁.
- 이 예제는 `examples/CMakeLists.txt`에서 **주석 처리로 비활성** 상태다. 작업 시작 시 `add_subdirectory(opcua_node)` 주석 해제가 첫 커밋이다.

### 0.3 아키텍처 사실 (설계 결정에 직접 영향)
1. **단일 스레드/단일 코어 협력 스케줄링.** `UA_MULTITHREADING=0` + main.c 폴링 루프. 듀얼코어 미사용. → A-2 동시성 규칙은 "단일 코어 협력 스케줄링 명시" 쪽이 현재 구조와 일치한다. Core1 동원은 선택이며, 하면 코어 간 큐가 필수다.
2. **값 노출은 DataSource 읽기 콜백(pull) 모델.** `UA_Server_addDataSourceVariableNode` + `opcua_read_value()`가 `data_table_snapshot()`을 읽는다. 노드에 값을 push하지 않는다. → A-1 동적 노드도 이 pull 모델을 유지하는 편이 메모리·동시성에서 유리하다 (콜백 컨텍스트 = 채널 인덱스).
3. **⚠ 세션/연결 한도: 현 트랜스포트는 하드웨어 소켓 0번 하나로 단일 TCP 연결만 수용한다** (`open62541_wiznet_eventloop.c:11` `WIZ_UA_SOCKET 0`, listen 1 + active 1). 지시서 원안의 "동시 세션 4"는 **eventloop 확장 없이는 불가능**하다. A-5에서 처리 방침 참조.
4. 서버 버퍼: `UA_ServerConfig_setMinimalCustomBuffer(..., 8192, 8192)` + eventloop RX 8192. SecurityPolicy None(minimal config 기본).
5. 네임스페이스 URI 현재값: `urn:WIZnet:EVB-PICO2:OpcUaUsbStdio` (`inc/opcua_server.h:7`). 엔드포인트 URL은 `opc.tcp://192.168.11.2:4840` 하드코딩 — IP가 settings로 바뀌면 이 URL도 동적 생성해야 한다.
6. W6300 chip init은 `wizchip_initialize()`의 무한 PHY 대기를 우회한 논블로킹 버전 (`wiznet_network.c:112-138`) — LAN 케이블 없이도 USB CLI가 살아있게 하려는 의도. 유지하라.

### 0.4 이식 자산 (같은 저장소, 검증 완료)
`examples/wiz_claw_spi_host/wiz_claw_webserver.c/.h`:
- flash settings 구조체 + 로드/세이브, 웹 설정 페이지(GET /), `POST /save` 파싱, 저장 후 watchdog 리부트(3s). 소켓 7, 포트 80, HTTP/1.0.
- **주의 1**: `wiz_claw_webserver.c:49`에 실물로 보이는 텔레그램 봇토큰 기본값이 하드코딩돼 있다. 이식 시 wiz_claw 전용 필드(봇토큰, LLM 키, vision 등)는 **가져오지 마라** — settings 스키마를 OPC UA 노드용으로 새로 정의한다.
- **주의 2**: 원본은 인증 없음 + 시크릿을 `value='%s'`로 HTML에 노출한다. A-4의 인증·마스킹 요구는 이 약점의 수정이다.
- **주의 3**: 원본에는 schema_version/CRC32가 없다 (magic만 있을 수 있음). A-4 요구사항은 신규 구현이다.

### 0.5 지시서 원안 대비 정정 사항
| 원안 서술 | 실제 |
|---|---|
| 파일이 예제 루트에 평면 배치 | `src/` + `inc/` 분리 |
| opcua_server.c 502줄 | 503줄 (무시 가능) |
| "노드맵은 opcua_node_map.h에 하드코딩" | 정확. 단 ID/브라우즈네임 실측: root `Sensor_Node`(ns=1;i=1000), `Device`(i=1001), `USB_Stdio`(i=2000), 변수는 string NodeId (`"Device.Name"` 등) |
| WIZ CLAW의 "settings schema_version + CRC32, 팩토리 리셋, 대시보드 인증" | wiz_claw에 **설계로만 존재**, 코드에는 미구현. Phase A에서 신규 작성 |
| 동시 세션 기본 4 | 현 트랜스포트 물리 한계 = 1 (0.3-3 참조) |

---

## 1. 제품 정의 (변경 금지)

- 정체성: RS-485(Modbus RTU)·UART 로우데이터를 OPC UA 주소공간으로 올려주는 초저가 변환 노드. 상위 시스템은 UAExpert 등 표준 클라이언트로 즉시 접속.
- 운용 모델: 사용자는 절대 컴파일하지 않는다. 고정 UF2 1개 + 런타임 프로비저닝(웹 대시보드 / USB-CDC CLI).
- 인입 모드 3종 (동시 활성 가능, 설정 on/off): `MODBUS_RTU`(주 판매 모드) / `UART_RAW`(구분자 기반, `$DATA:` 프리셋 유지) / `USB_STDIO`(디버그·교육, 현행 보존).
- SecurityPolicy: Phase A는 None 유지 허용. 단 settings 스키마와 UI에 Phase B 보안 자리(placeholder, "Phase B에서 활성화" 표기) 선반영.
- Phase B(Basic256Sha256+인증), Phase C(PubSub/멀티세션)를 막는 설계 금지. 특히 amalgamation 플래그 변경으로 해결될 일(PubSub 등)은 재생성 절차만 문서화해두면 된다.

---

## 2. Phase A 태스크

**순서 고정: A-0 → A-1 → A-4(settings 코어만 선행 가능) → A-2 → A-3 → A-5 → A-6.**
A-1과 A-4의 settings 저장소는 상호 의존하므로, 먼저 settings 코어(구조체+flash+CRC)를 만들고 A-1이 그것을 소비하는 순서를 권장한다.
각 태스크의 수용 기준을 만족하기 전에 다음 태스크로 넘어가지 마라.

### A-0. 빌드 활성화 (선행)
- `examples/CMakeLists.txt`의 `# add_subdirectory(opcua_node)` 주석 해제.
- 원본과 동일하게 빌드·플래시·UAExpert 접속이 되는지 확인 (기준선). 이 시점의 map 파일/힙 여유를 기록 — memory_budget의 "빈 서버" 기준점.

### A-1. 동적 노드맵 (최우선 — 다른 태스크의 기반)
- `opcua_node_map.h` 하드코딩 제거. 채널 정의를 settings에 저장, 부팅 시 주소공간 동적 생성.
- 채널 정의 스키마 (채널당):
  ```c
  typedef struct {
      uint16_t id;
      char     name[32];
      uint8_t  source;         /* MODBUS | UART | USB */
      uint8_t  datatype;       /* Float | Int32 | Bool | String */
      char     unit[16];
      float    scale, offset;
      /* MODBUS 전용 */
      uint8_t  slave_addr;
      uint8_t  function_code;  /* 3|4|1|2 */
      uint16_t register_addr;
      uint16_t register_count;
      uint8_t  word_order;
  } channel_def_t;
  ```
- 최대 채널: 메모리 예산 실측 후 결정하되 **최소 32 보장**. 채널 수 × open62541 노드 메모리를 계측해 `docs/memory_budget.md`에 기록 (0.2의 amalgamation 플래그 목록 포함).
- 주소공간: `Objects/<DeviceName>/Device/{진단}` + `Objects/<DeviceName>/Channels/<name>` (EngineeringUnits, EURange 프로퍼티 포함 — NS0 MINIMAL에 해당 타입 노드가 있는지 먼저 확인하고, 없으면 amalgamation 플래그 조정 또는 Property 노드로 대체하고 결정을 문서화하라). 네임스페이스 URI `urn:wiznet:opcua-node` 고정 (현재값에서 변경).
- 값 노출은 현행 DataSource pull 모델 유지 권장 (0.3-2). data_table을 3채널 고정에서 채널 배열로 재설계.
- 진단 노드: 인입 모드별 FrameCount/ErrorCount, ModbusTimeoutCount, LinkStatus, FreeHeap, RebootCount, RebootReason(watchdog/power/user).
- **수용 기준**: 32채널 정의로 부팅·UAExpert 전 채널 브라우징 정상. 엔드포인트 URL이 실제 IP를 반영.

### A-2. Modbus RTU 마스터 (RS-485 인입)
- RP2350 UART1 + DE/RE 방향 제어 GPIO, half-duplex 마스터. 보레이트/패리티/스톱 설정화 (기본 9600 8N1). **핀 배치는 W6300_EVB_PICO2에서 QSPI(W6300)·USB와 충돌하지 않는 UART1 핀으로 선정하고 헤더에 핀맵 표를 남겨라.**
- 폴링 스케줄러: (slave, fc, addr) 조합을 요청 단위로 병합·그룹화해 버스 왕복 최소화. 폴링 주기 설정화 (기본 1s, 채널별 오버라이드).
- CRC16 검증, 타임아웃(설정, 기본 200ms) + 재시도 1회. 슬레이브 연속 실패 시 해당 채널 StatusCode = `Bad_CommunicationError` (값 유지 금지 — DataSource 콜백에서 `value->hasStatus` 경로로 정직하게 노출. 현행 `opcua_read_value`의 에러 패턴 재사용).
- 동시성: **단일 코어 협력 스케줄링을 기본으로 한다** (0.3-1). 메인 루프에 modbus_poll() 단계 추가, 블로킹 대기 금지(상태머신). Core1 사용은 금지하지 않으나 선택 시 코어 간 SPSC 큐 + 문서화 필수.
- **수용 기준**: 실제 또는 시뮬레이터 슬레이브에서 FC3/FC4 읽기 → scale/offset → UAExpert 값 확인. 슬레이브 전원 차단 시 해당 채널만 Bad, 복구 시 자동 정상화.

### A-3. UART_RAW 범용 파서
- 설정: 시작 토큰(기본 `$DATA:`), 필드 구분자(기본 `,`), 종료(기본 CR/LF), 필드→채널 매핑.
- 파싱 실패: ParseErrorCount 증가 + 마지막 오류 프레임을 RawFrame 진단 노드에 보존.
- **회귀 금지**: 현행 USB_STDIO 명령(`GET`/`NET`/`OPCUA`/`CLEAR`/`HELP`, `?` 별칭, `DATA:` 무접두 변형 포함)과 README의 Tera Term + UaExpert 절차 100% 동작 유지.
- **수용 기준**: 기본 프리셋으로 기존 데모 절차 통과 + 커스텀 구분자 설정으로 별도 포맷 1종 수신 확인.

### A-4. 프로비저닝/설정 (wiz_claw 자산 이식·확장)
- settings 코어 (신규 — 0.4 주의 3): 구조체에 `magic + schema_version(u16) + CRC32`. 불일치 시 기본값 롤백. 마이그레이션 훅 골격(`settings_migrate(old_ver, blob)`) 포함. Phase B 보안 필드(정책 enum, 인증서 슬롯 오프셋) placeholder 예약.
- 네트워크: 첫 부팅(또는 settings 무효) 시 **DHCP 기본** → 할당 IP를 USB-CDC로 출력. DHCP 실패 시 기본 정적 IP(192.168.11.2) 폴백 + CLI 안내. DHCP 소켓은 OPC UA(0)·웹서버(7)와 충돌하지 않게 배치하고 **소켓 배치표를 헤더 주석으로 명시** (참고: W6300도 8소켓).
- USB-CDC CLI: `ip/gw/sn/dns/dhcp on|off/show/reset/reboot` 최소셋 + `stats`(A-6). 기존 usb_stdio_link.c의 라인 파서를 확장하되 **웹과 CLI는 동일 settings API 사용** (이중 구현 금지).
- 웹 대시보드: wiz_claw_webserver 이식, 탭 [네트워크] [채널 맵] [인입 설정(Modbus/UART)] [진단] [시스템]. 인증 = 비밀번호 + 세션 토큰(쿠키), 최초 로그인 시 비밀번호 강제 변경. 시크릿·비밀번호는 응답 HTML에 절대 미포함(마스킹). Phase B 보안 설정 자리는 비활성 표기.
- 저장 반영: 채널 맵 변경은 **핫 리로드 우선**(주소공간 재구성 — 서버 유지, 노드만 삭제/재생성. `UA_ENABLE_NODEMANAGEMENT=ON`이라 가능), 네트워크 설정 등 불가 항목만 리부트.
- 팩토리 리셋: 부팅 시 지정 GPIO 3초 홀드 → settings 소거 (GPIO 선정 시 핀맵 표에 추가).
- **수용 기준**: 컴파일 없이 UF2 플래시 → DHCP IP 획득 → 웹 로그인 → 채널 정의 → UAExpert 확인 흐름 성립.

### A-5. 신뢰성
- 하드웨어 watchdog 활성. 모든 메인 루프 단계가 heartbeat 비트마스크에 참여, 전 비트 세트 시에만 kick.
- RebootCount/RebootReason flash 기록 + 진단 노드 노출.
- 세션/구독 한도: **현 트랜스포트는 단일 연결이다 (0.3-3).** Phase A 처리 방침 — (a) eventloop를 다중 소켓 accept로 확장해 동시 세션 N(목표 4, 소켓 예산 내) 지원, 또는 (b) 한도 1을 명시하고 초과 연결이 정상 거절(크래시·행 없음)되는지 검증 후 멀티세션을 Phase C로 이관. **(a) 시도 → 2일 내 미완이면 (b)로 전환하고 결정을 docs에 기록.** 구독 한도(기본 8)는 서버 config로 설정.
- 24h soak 절차 작성: UaExpert 구독 250ms × 8노드 + Modbus 1s 폴링, FreeHeap 단조 감소(누수) 없음 검증. 절차는 `docs/soak_test.md`.
- **수용 기준**: soak 24h 재부팅 0, 힙 누수 0, 카운터 정상 집계. 한도 초과 접속 정상 거절.

### A-6. 계측 및 문서
- 부팅→서버 RUNNING 시간, Modbus 폴→노드 갱신 지연, 구독 통지 지연을 진단 노드 또는 CLI `stats`로 노출.
- 산출물: `docs/node_map.md`(채널 스키마·주소공간 스펙), `docs/provisioning.md`(사용자 가이드, 스크린샷 자리), `docs/modbus_mapping.md`(폴링 테이블 가이드 + 온습도 센서·전력미터 예시), `docs/memory_budget.md`(채널×메모리 실측 + amalgamation 플래그), `docs/soak_test.md`.
- 로그는 "15분 셋업" 실측 영상 캡처가 가능하도록 깔끔하게 (기동 배너 → DHCP IP → 대시보드 URL 순서로 명확히).

---

## 3. Phase A 완료 조건 (전체)

- [ ] 컴파일 없이: UF2 플래시 → DHCP IP → 웹 로그인 → 채널 8개(Modbus 4 + UART 4) 정의 → UAExpert 전 채널 실시간 확인, 15분 이내.
- [ ] Modbus 슬레이브 전원 차단 → 해당 채널만 Bad 품질코드, 복구 시 자동 정상화.
- [ ] 기존 USB_STDIO 데모 절차 100% 회귀 통과.
- [ ] 24h soak: 재부팅 0, 힙 누수 0, ParseError/Timeout 카운터 정상.
- [ ] 32채널 부팅·브라우징 정상, memory_budget 실측 일치.
- [ ] settings에 Phase B 보안 placeholder 존재 + UI 비활성 표기.
- [ ] 세션 한도 방침 결정·검증·문서화 (0.3-3).

## 4. 작업 규칙

- 커밋은 A-x 태스크 단위, 메시지에 태스크 ID 포함 (예: `feat(opcua_node): A-1 dynamic node map`).
- `examples/opcua_usb_stdio/` 원본 수정 금지. `port/open62541/` 수정은 opcua_usb_stdio 빌드를 깨지 않는 범위에서만 (A-5(a)의 eventloop 확장 포함 — 커스텀 포인트는 컴파일 플래그나 신규 파일로).
- open62541 amalgamation 플래그를 바꾸면 반드시 `docs/memory_budget.md`에 before/after 기록.
- 모르는 것·전제 불일치 발견 시 추측 구현 금지, 질문 목록으로 보고.
- 빌드 확인: 루트에서 `cmake -B build_W6300 && cmake --build build_W6300 --target opcua_node` (보드는 루트 CMakeLists의 W6300_EVB_PICO2 그대로).

## 5. 착수 전 확인 질문 (답 없으면 기본값으로 진행)

1. RS-485 트랜시버 하드웨어: 어떤 breakout/트랜시버(예: MAX3485)를 어느 핀에? — 기본값: UART1 TX/RX + DE/RE 겸용 1핀, 핀 번호는 A-2에서 충돌 검사 후 제안.
2. 채널 맵 웹 UI: 테이블 편집(행 추가/삭제) 수준이면 되는가, CSV 임포트도 필요한가? — 기본값: 테이블 편집만.
3. 대시보드 초기 비밀번호 정책: 라벨 인쇄용 디바이스 고유값(MAC 기반) vs 고정 기본값+강제변경? — 기본값: 고정 `admin` + 최초 로그인 강제 변경.
4. soak 테스트에 쓸 Modbus 슬레이브: 실물 센서 보유? 없으면 PC 시뮬레이터(diagslave 등) 기준으로 절차 작성.
