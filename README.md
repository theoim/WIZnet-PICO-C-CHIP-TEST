# WIZnet PICO C Chip Test Lab

이 저장소는 WIZnet Ethernet 칩과 Raspberry Pi Pico/Pico2 계열 보드를 테스트하고, 그 위에서 다양한 네트워크 애플리케이션을 실험하기 위해 포크한 개발용 레포지토리입니다.

원본 WIZnet-PICO-C 예제의 보드 포팅, ioLibrary, Pico SDK 구성을 기반으로 하되, 목적은 단순한 예제 실행에 그치지 않습니다. 칩 동작 확인, 인터페이스 검증, 네트워크 기능 테스트, OPC UA, AI Agent 같은 응용 프로젝트까지 확장하는 실험 공간입니다.

## 프로젝트 목표

- W5100S, W5500, W6100, W6300 등 WIZnet Ethernet 칩 동작 검증
- SPI/QSPI, DMA, PIO, GPIO interrupt 등 보드 포팅 코드 확인
- Pico/Pico2 기반 네트워크 예제 제작
- W6300 기반 산업용/응용 프로토콜 실험
- OPC UA, Telegram/OpenAI Agent 같은 상위 애플리케이션 프로토타입 개발
- 칩 테스트 코드와 실제 응용 예제를 같은 레포에서 관리

## 기본 보드 설정

보드 선택은 루트 `CMakeLists.txt`의 `BOARD_NAME`에서 설정합니다.

```cmake
set(BOARD_NAME W6300_EVB_PICO2)
```

현재 개발 기준은 주로 `W6300_EVB_PICO2`입니다. 다른 보드를 사용할 경우 루트 `CMakeLists.txt`의 보드 선택 영역에서 대상 보드로 변경하세요.

W6300 계열은 QSPI 모드를 사용합니다.

```cmake
set(WIZCHIP_QSPI_MODE QSPI_QUAD_MODE)
```

필요하면 `QSPI_DUAL_MODE`, `QSPI_SINGLE_MODE`로 변경해서 테스트할 수 있습니다.

## 예제 목록

현재 `examples/` 아래의 예제들은 다음 목적을 가집니다.

| 예제 | 설명 |
| --- | --- |
| `buffer_read` | WIZnet 칩의 RX/TX 버퍼 접근과 데이터 읽기 동작을 확인하는 저수준 테스트 예제 |
| `PING_TEST` | 네트워크 설정 후 ICMP Ping 송수신을 확인하는 기본 연결성 테스트 |
| `10M_Complience_Test` | 10Mbps Ethernet 동작과 compliance 관련 확인을 위한 칩 테스트 예제 |
| `loopback` | TCP/UDP loopback으로 소켓 송수신 기본 동작을 검증하는 예제 |
| `opcua_usb_stdio` | USB CDC 입력 데이터를 OPC UA 서버 노드로 노출하는 W6300 기반 OPC UA 프로토타입 |
| `wiz_claw_agent` | Telegram Bot 메시지를 받고 OpenAI API와 GPIO tool call을 연동하는 WIZnet 기반 AI Agent 예제 |

빌드할 예제는 `examples/CMakeLists.txt`에서 `add_subdirectory(...)`로 관리합니다.

```cmake
add_subdirectory(buffer_read)
add_subdirectory(PING_TEST)
add_subdirectory(10M_Complience_Test)
add_subdirectory(opcua_usb_stdio)
add_subdirectory(wiz_claw_agent)
```

특정 예제만 빌드하고 싶다면 나머지 줄을 주석 처리하면 됩니다.

## 디렉터리 구조

```text
.
├── examples/              # 칩 테스트 및 응용 예제
├── libraries/             # pico-sdk, ioLibrary_Driver, mbedtls 등 외부 라이브러리
├── port/                  # 보드/칩 의존 포팅 코드
│   ├── ioLibrary_Driver/  # WIZnet 칩 SPI/QSPI/GPIO 포팅
│   ├── mbedtls/           # TLS 설정
│   ├── open62541/         # OPC UA 스택
│   ├── timer/             # 타이머 포팅
│   └── wiz-claw/          # Telegram/OpenAI Agent용 네트워크/HTTP/TLS/LLM 모듈
└── CMakeLists.txt         # 보드 선택 및 전체 빌드 구성
```

## 빌드 환경

이 프로젝트는 Raspberry Pi Pico SDK 기반 CMake 프로젝트입니다. 개발은 주로 Windows + VS Code + CMake Tools 환경을 기준으로 합니다.

필요한 구성:

- Raspberry Pi Pico SDK
- ARM GNU Toolchain
- CMake
- Ninja 또는 NMake
- VS Code CMake Tools 확장
- WIZnet 보드 또는 Pico/Pico2 + WIZnet Ethernet 모듈

서브모듈이 비어 있다면 다음처럼 가져옵니다.

```powershell
git submodule update --init --recursive
```

## 포팅 코드

칩 및 보드 의존 코드는 `port/` 아래에서 관리합니다.

| 경로 | 역할 |
| --- | --- |
| `port/ioLibrary_Driver` | SPI/QSPI, DMA, GPIO interrupt, WIZchip 초기화 |
| `port/mbedtls` | TLS 사용을 위한 mbedTLS 설정 |
| `port/open62541` | OPC UA 서버 구현을 위한 open62541 |
| `port/timer` | WIZnet 라이브러리용 타이머 |
| `port/wiz-claw` | Telegram, HTTP/TLS, OpenAI-compatible LLM, Agent loop |

W6300 QSPI 관련 코드는 주로 `port/ioLibrary_Driver/src/wizchip_qspi_pio.c`와 `.pio` 파일에서 확인할 수 있습니다.

## 응용 예제 메모

`opcua_usb_stdio`는 W6300의 TCP/IP 기능 위에서 open62541 기반 OPC UA 서버를 구동하는 예제입니다. USB CDC로 입력한 데이터를 내부 데이터 테이블에 저장하고, OPC UA 클라이언트가 해당 값을 노드로 읽을 수 있게 합니다.

`wiz_claw_agent`는 WIZnet 칩으로 TLS 연결을 수행하고 Telegram Bot API와 OpenAI API를 호출합니다. Telegram 메시지를 LLM에 전달하고, LLM이 GPIO tool call을 요청하면 보드의 LED 같은 GPIO를 제어합니다.

## 원본 프로젝트

이 저장소는 WIZnet-PICO-C 기반으로 포크한 개발용 레포지토리입니다. 원본 프로젝트는 WIZnet Ethernet 칩과 Raspberry Pi Pico 계열 보드를 위한 기본 예제와 포팅 코드를 제공합니다.

이 레포에서는 원본 구성을 바탕으로 칩 테스트와 응용 프로젝트 개발에 필요한 코드를 추가하고 정리합니다.
