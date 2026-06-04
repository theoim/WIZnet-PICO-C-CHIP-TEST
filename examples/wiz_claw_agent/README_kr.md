# wiz_claw_agent 예제

`wiz_claw_agent`는 RP2350 보드와 WIZnet Ethernet 칩을 사용해서 Telegram Bot 메시지를 받고, OpenAI Chat Completions API로 응답을 만든 뒤, 필요하면 GPIO 제어 tool을 실행하는 예제입니다.

현재 예제의 기본 tool은 `set_gpio` 하나입니다. 사용자가 Telegram으로 LED 제어를 요청하면 LLM이 `set_gpio` tool call을 만들고, 보드가 실제 GPIO를 켜거나 끈 다음 결과를 다시 Telegram으로 보냅니다.

## 동작 흐름

1. USB CDC stdio를 초기화하고 로그를 출력합니다.
2. 보드 LED GPIO를 출력으로 설정합니다.
3. WIZnet 칩을 SPI/QSPI로 초기화합니다.
4. DNS 모듈과 1초 타이머를 초기화합니다.
5. 정적 IP로 네트워크를 설정합니다.
6. Telegram `getUpdates`를 long-polling 방식으로 반복 호출합니다.
7. 새 Telegram 메시지가 오면 OpenAI API에 전달합니다.
8. LLM이 tool call을 요청하면 `gpio_tool_handler()`가 GPIO를 제어합니다.
9. 최종 답변을 Telegram `sendMessage`로 전송합니다.

## 네트워크 설정

`main.c`의 `g_net_info`에서 IP 정보를 설정합니다.

```c
static wiz_NetInfo g_net_info = {
    .mac  = { 0x00, 0x08, 0xDC, 0xAB, 0xCD, 0xEF },
    .ip   = { 192, 168, 11, 20 },
    .sn   = { 255, 255, 255, 0 },
    .gw   = { 192, 168, 11, 1 },
    .dns  = { 8, 8, 8, 8 },
    .dhcp = NETINFO_STATIC,
};
```

현재 코드는 정적 IP를 사용합니다. PC 또는 공유기 환경에 맞게 `ip`, `sn`, `gw`, `dns`를 수정하세요.

## API 설정

빌드 전에 `main.c` 상단의 값을 실제 값으로 바꿔야 합니다.

```c
#define BOT_TOKEN       "xxxxx"
#define OPENAI_API_KEY  "xxxx"
#define AGENT_MODEL     "gpt-4o-mini"
```

- `BOT_TOKEN`: Telegram BotFather에서 발급받은 Bot Token
- `OPENAI_API_KEY`: OpenAI API key
- `AGENT_MODEL`: 사용할 OpenAI 모델 이름

키가 소스에 직접 들어가므로, 공개 저장소에 커밋하지 않도록 주의하세요.

## 소켓 사용

예제는 WIZnet 소켓 번호를 용도별로 나눠 사용합니다.

| 소켓 | 용도 |
| --- | --- |
| 0 | Telegram `getUpdates` long-poll |
| 1 | Telegram `sendMessage` |
| 2 | OpenAI LLM API |
| 5 | DNS |
| 6 | DHCP 예약 |

`main.c`에는 DHCP 소켓이 `0`으로 설정되어 있지만, 현재 `g_net_info.dhcp = NETINFO_STATIC`이므로 DHCP는 실제로 사용하지 않습니다. DHCP를 켤 경우 Telegram polling 소켓과 충돌하지 않도록 소켓 번호를 다시 배치해야 합니다.

## GPIO Tool

LLM에 노출되는 tool 이름은 `set_gpio`입니다.

Tool arguments 형식은 다음과 같습니다.

```json
{
  "pin": 25,
  "state": "on"
}
```

`state`는 다음 값 중 하나입니다.

| 값 | 동작 |
| --- | --- |
| `on` | GPIO High |
| `off` | GPIO Low |
| `toggle` | 현재 상태 반전 |

기본 LED 핀은 `PICO_DEFAULT_LED_PIN`입니다. Pico 계열 보드에서는 일반적으로 GPIO 25입니다.

## Telegram에서 테스트하기

펌웨어를 보드에 올리고 USB 시리얼 로그에서 다음 메시지가 보이면 polling이 시작된 상태입니다.

```text
=== wiz_claw_agent starting ===
Telegram polling start...
```

Telegram 봇에게 다음처럼 메시지를 보내 테스트할 수 있습니다.

```text
LED 켜줘
LED 꺼줘
GPIO 25 토글해줘
```

정상 동작하면 USB 로그에 Telegram 수신 메시지, tool 실행 로그, 최종 reply가 출력됩니다.

## 빌드 관련 주의사항

이 예제는 TLS 통신을 사용하므로 mbedTLS 설정에 entropy 모듈이 필요합니다. 링크 단계에서 아래와 같은 에러가 나오면:

```text
undefined reference to `mbedtls_entropy_init'
undefined reference to `mbedtls_entropy_add_source'
undefined reference to `mbedtls_entropy_func'
undefined reference to `mbedtls_entropy_free'
```

`port/mbedtls/inc/ssl_config.h`에 다음 정의가 있는지 확인하세요.

```c
#define MBEDTLS_ENTROPY_C
```

RP2350 TRNG를 entropy source로 추가하는 구조라면 `MBEDTLS_NO_PLATFORM_ENTROPY`는 유지해도 됩니다.

## 관련 파일

| 파일 | 설명 |
| --- | --- |
| `examples/wiz_claw_agent/main.c` | 예제 진입점, 네트워크/API/tool 설정 |
| `examples/wiz_claw_agent/CMakeLists.txt` | 예제 빌드 타겟 |
| `port/wiz-claw/src/wiz_claw_telegram.c` | Telegram Bot API 처리 |
| `port/wiz-claw/src/wiz_claw_llm.c` | OpenAI-compatible LLM 요청/응답 처리 |
| `port/wiz-claw/src/wiz_claw_agent.c` | LLM과 tool call 반복 실행 |
| `port/wiz-claw/src/wiz_claw_http.c` | WIZnet 기반 HTTP/TLS 전송 |
| `port/wiz-claw/src/wiz_claw_tls.c` | mbedTLS 기반 TLS 처리 |
