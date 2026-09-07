# baseline.md — 측정된 기준선

> **이 문서는 사실만 담는다.** 결정이나 목표는 여기에 쓰지 않는다.
> 모든 수치는 8장 절차로 재현 가능하다.

측정일: 2026-09-03 · 브랜치 `main` · 커밋 `7ff1f56`

---

## 1. 측정 조건

| 항목 | 값 |
|---|---|
| 리포 | `D:\theo_git_project\WIZnet-PICO-C-CHIP-TEST` |
| 브랜치 | `main` |
| BOARD_NAME | `W6300_EVB_PICO2` (main 기본값, 변경 없음) |
| 타깃 | RP2350 + W6300 (QSPI QUAD), `-D_WIZCHIP_=W6300` |
| 툴체인 | arm-none-eabi-gcc 10.3.1 (GNU Arm Embedded 10.3-2021.10) |
| pico-sdk | `libraries/pico-sdk` = **2.3.0** |
| CMake | 3.29.2, generator `MinGW Makefiles` |
| **CMAKE_BUILD_TYPE** | **Release** (지정하지 않았을 때의 기본값) |
| 빌드 디렉터리 | `build_opcua_main/` |

**빌드 타입 주의.** 이전에 쓰던 `build/`는 **Debug**였다. 같은 코드라도 수치가 크게
다르므로 비교할 때 반드시 확인할 것. 이 문서의 모든 수치는 **Release** 기준이다.

---

## 2. 측정 대상 — 두 예제 모두 빌드됨

| 예제 | 성격 |
|---|---|
| `opcua_node` | Phase A 제품 프로토타입. 동적 채널 + 진단 노드 15종 |
| `opcua_usb_stdio` | 회귀 기준(레퍼런스). 3채널 고정 |

두 예제는 `if(NOT TARGET OPEN62541_WIZNET)` 가드 추가 후 **동시 활성화 가능**하다
(9-4 참조). `OPEN62541_WIZNET`은 한 번만 빌드되어 둘이 공유한다.

---

## 3. FLASH

| 섹션 | `opcua_node` | `opcua_usb_stdio` | 차이 |
|---|---:|---:|---:|
| `.text` | 238,880 | 235,312 | +3,568 |
| `.rodata` | 58,724 | 58,700 | +24 |
| `.data` (플래시 적재 초기값) | 54,344 | 54,240 | +104 |
| `.ARM.exidx` + `.binary_info` | 44 | 44 | 0 |
| **합계** | **351,992 B (343.7 KB)** | **348,296 B (340.1 KB)** | **+3,696** |
| `.uf2` | 705,024 B | 697,344 B | +7,680 |

4 MB 플래시 기준 사용률: **8.4 %** / 8.3 %

---

## 4. RAM (정적)

| 섹션 | `opcua_node` | `opcua_usb_stdio` | 차이 |
|---|---:|---:|---:|
| `.ram_vector_table` | 272 | 272 | 0 |
| `.data` | 54,344 | 54,240 | +104 |
| `.bss` | **10,292** | 3,576 | **+6,716** |
| **정적 합계** | **64,908 B (63.4 KB)** | **58,088 B (56.7 KB)** | **+6,820** |
| RP2350 SRAM 520 KB 잔여 | **456.6 KB** | 463.3 KB | −6.7 KB |

### 4.1 `.bss` 증가분은 전부 설명된다

`opcua_node`에만 있는 `.bss` 심볼:

| 심볼 | 크기 (B) | 정체 |
|---|---:|---|
| `s_values` | **3,840** | `channel_value_t` × 32채널 (120 B × 32) |
| `g_settings` | **2,816** | `opcua_settings_t` 전체 |
| `s_diag` | 120 | 진단 스냅샷 |
| `s_endpoint` | 48 | 동적 엔드포인트 URL 문자열 |
| 기타 (`s_cfg` 등) | 14 | 포인터 |
| **소계** | **6,838** | `.bss` 차이 6,716과 일치 |

**`memory_budget.md`의 예측이 실측과 맞았다.**

| 항목 | 예측 (memory_budget.md) | 실측 |
|---|---|---|
| `opcua_settings_t` | 2,816 B | **2,816 B** ✔ |
| `channel_value_t` × 32 | "~120 B × 32 = ~3.8 KB" | **3,840 B** ✔ |

### 4.2 `.data`가 54 KB인 이유 — `UA_TYPES`

```
20005604 g  O .data  00006d20  UA_TYPES
```

**open62541의 `UA_TYPES` 테이블 27,936 B (27.3 KB)가 `.data`, 즉 RAM에 있다.**
`opcua_node` 정적 RAM의 **43 %**를 이 하나가 차지한다. 두 예제 모두 동일하다.

읽기 전용 성격의 테이블이므로 `.rodata`로 옮길 수 있는지는 **확인하지 않았다.**
amalgamation 생성 방식과 관련이 있을 가능성이 있으나 근거 없음.

### 4.3 이름 있는 정적 심볼 상위 (`opcua_node`)

| 심볼 | 크기 (B) | 출처 |
|---|---:|---|
| `s_values` | 3,840 | `data_table.c` |
| `g_settings` | 2,816 | `opcua_settings.c` |
| `hw_endpoints` | 1,024 | TinyUSB |
| `_vendord_itf` | 588 | TinyUSB vendor class |
| `default_alarm_pool_entries` | 384 | pico-sdk |
| `ram_vector_table` | 272 | pico-sdk |
| `_cdcd_itf` | 200 | TinyUSB CDC |
| `_usbd_qdef_buf` | 192 | TinyUSB |
| `s_diag` | 120 | `opcua_server.c` |

이름 있는 심볼 합계는 1만 B 안팎이다. 나머지 `.data` 대부분은 `UA_TYPES`를 포함한
open62541 내부 테이블이다.

---

## 5. 소켓과 버퍼

| 항목 | 값 | 출처 |
|---|---|---|
| 사용 하드웨어 소켓 | **1개** (`WIZ_UA_SOCKET 0`) | `port/open62541/open62541_wiznet_eventloop.c:11` |
| listen connection id | 1 | 같은 파일 `:13` |
| active connection id | 2 | 같은 파일 `:14` |
| eventloop RX 버퍼 | 8,192 B | 같은 파일 `:15`, `rxBuffer[]` `:49` |
| 서버 send / recv 버퍼 | 8,192 / 8,192 | `opcua_server.c` `UA_ServerConfig_setMinimalCustomBuffer` |

**동시 TCP 연결은 구조상 1개다.** W6300 하드웨어 소켓은 8개이므로 여유는 있으나
현재 이벤트루프가 0번 하나만 쓴다. 버퍼만으로 이미 **24 KB** (8192 × 3).

---

## 6. open62541 amalgamation 플래그

`port/open62541/CMakeLists.txt:17-29` — 두 브랜치 동일.

| 플래그 | 값 |
|---|---|
| `UA_ARCHITECTURE` | `none` |
| `UA_NAMESPACE_ZERO` | `MINIMAL` |
| `UA_ENABLE_METHODCALLS` | ON |
| `UA_ENABLE_SUBSCRIPTIONS` | ON |
| `UA_ENABLE_SUBSCRIPTIONS_EVENTS` | OFF |
| `UA_ENABLE_HISTORIZING` | OFF |
| `UA_ENABLE_PUBSUB` | OFF |
| `UA_ENABLE_DISCOVERY` | OFF |
| `UA_ENABLE_NODEMANAGEMENT` | ON |
| `UA_MULTITHREADING` | 0 |
| **`UA_ENABLE_ENCRYPTION`** | **플래그 자체가 없음 → 암호화 비활성** |

컴파일 정의: `UA_ARCHITECTURE_LWIP`, `UA_LOGLEVEL=300`

---

## 7. 측정하지 못한 것

| 항목 | 왜 |
|---|---|
| heap peak | 실기 실행 필요. `Device/FreeHeap` 노드를 UAExpert나 USB `GET`으로 읽어야 함 |
| stack high-water | `-fstack-usage` 또는 실기 워터마크 필요 |
| 채널 수별 heap 증가 | `memory_budget.md`의 0 / 8 / 16 / 32 표. **실기 필요** |
| `UA_TYPES`를 `.rodata`로 옮길 수 있는지 | 미조사 |
| 보안 활성화 시 증가량 | `UA_ENABLE_ENCRYPTION` 플래그 자체가 없음 |
| RSA-2048 연산 시간 | 암호화 미활성 |
| Debug 빌드 수치 | Release만 측정 |

**정적 RAM은 여유가 크다 (456.6 KB).** 병목은 메모리가 아니라 5장의 단일 소켓일
가능성이 높으나, heap peak를 재기 전에는 단정할 수 없다.

---

## 8. 재현 절차

```bash
cd D:/theo_git_project/WIZnet-PICO-C-CHIP-TEST

cmake -S . -B build_opcua_main -G "MinGW Makefiles" \
  -DCMAKE_MAKE_PROGRAM="C:/PROGRA~2/MINGW-~1/bin/MINGW3~1.EXE" \
  -DPICO_SDK_PATH="D:/theo_git_project/WIZnet-PICO-C-CHIP-TEST/libraries/pico-sdk"

cd build_opcua_main
"C:/PROGRA~2/MINGW-~1/bin/MINGW3~1.EXE" opcua_node opcua_usb_stdio -j4

cd examples/opcua_node
arm-none-eabi-size -A -d opcua_node.elf
arm-none-eabi-nm --size-sort -S -r -td opcua_node.elf | awk '$3 ~ /^[BbDd]$/'
arm-none-eabi-objdump -t opcua_node.elf | grep -w UA_TYPES
```

BOARD_NAME 변경 불필요. `examples/CMakeLists.txt`에서 두 예제 모두 활성화된 상태로
그대로 빌드된다.

---

## 9. 빌드 함정 (이 측정에서 실제로 걸린 것)

**9-1. `CMAKE_MAKE_PROGRAM` 경로에 공백이 있으면 pioasm 빌드가 깨진다**

```
'C:/Program'은(는) 내부 또는 외부 명령... 이 아닙니다
```

pico-sdk의 `pioasmBuild` ExternalProject가 make 경로를 인용부호 없이 넘긴다.
**8.3 단축 경로를 쓸 것.**

```
C:/PROGRA~2/MINGW-~1/bin/MINGW3~1.EXE     ← 이렇게
"C:/Program Files (x86)/mingw-w64/..."    ← 이러면 실패
```

단축 경로 확인:
```powershell
(New-Object -ComObject Scripting.FileSystemObject).GetFile("<전체경로>").ShortPath
```

**9-2. pico-sdk가 두 개다**

| 경로 | 버전 |
|---|---|
| 환경변수 `PICO_SDK_PATH` = `D:\RP2040\pico-sdk` | 2.1.0 |
| 리포 내 `libraries/pico-sdk` | **2.3.0** |

**`-DPICO_SDK_PATH=`를 반드시 명시할 것.** 안 하면 환경변수의 2.1.0으로 빌드된다.

**9-3. `nmake`는 Git Bash 셸에 없다**

Visual Studio 개발자 프롬프트가 아니면 `mingw32-make`를 써야 하고, generator가
달라 기존 빌드 디렉터리를 공유할 수 없다.

**9-4. 두 OPC UA 예제가 같은 바이너리 디렉터리를 쓴다**

가드 추가 전에는 둘을 동시에 켜면 설정이 실패했다.

```
CMake Error: The binary directory build/port/open62541
is already used to build a source directory.
```

각 예제 CMakeLists에 가드를 넣어 해결했다.

```cmake
if(NOT TARGET OPEN62541_WIZNET)
    add_subdirectory(${CMAKE_SOURCE_DIR}/port/open62541
        ${CMAKE_BINARY_DIR}/port/open62541)
endif()
```

**9-5. `.gitignore`에 `build/`만 있었다**

`build_*` 디렉터리가 추적 대상이 되어 빌드 산출물 5,859개가 스테이징된 적이 있다.
`build*/` 추가로 해결했다.

---

## 10. 이 측정 과정에서 고친 것

| 항목 | 내용 |
|---|---|
| `inc/opcua_node_map.h:13` | 주석 안의 `Usb*/`가 블록 주석을 조기 종료시켜 컴파일 오류 51개 발생. `Usb / Uart / Modbus`로 수정 |
| `examples/*/CMakeLists.txt` | 9-4의 가드 추가 |
| `.gitignore` | `build*/` 추가 |

`opcua_node`는 이 수정 전까지 **빌드된 적이 없다.**
