# RSA / OPC UA 암호 연산 비용 — 실측

**측정일:** 2026-10-15
**측정자:** 실기 4회 (RP2040 2회, RP2350 2회)
**도구:** `examples/crypto_bench`, `tools/bench_matrix.sh`
**저장소 상태:** `67fc400` + 미커밋 변경 (`crypto_bench/`, `tools/`, `ssl_config.h`, `CMakeLists.txt`)

---

## 1. 결론

**RP2350으로 간다. Basic256Sha256 + SignAndEncrypt는 성립한다.**

| 부품 | OpenSecureChannel 암호 연산 합계 | 클라이언트 5초 타임아웃 여유 |
|---|---|---|
| RP2040 @200MHz, 어셈블리 | 5,051 ms | **−51 ms** (불가) |
| RP2350 @150MHz, 어셈블리 | **1,229 ms** | **+3,771 ms** (가능) |

**RP2040은 어떤 설정으로도 통과하지 못한다.** 클럭은 이미 200 MHz이고 어셈블리도
켠 상태가 최선이다. RP2350은 정격 150 MHz, 스톡 상태로 4.1배 빠르다.

`MBEDTLS_HAVE_ASM`은 **두 부품 모두에서 켠다.** 1.38~1.64배 빠르고 `.text`는 오히려
568~1,072 B 작다. 비용이 없다.

---

## 2. 측정 환경

4개 이미지를 한 스크립트로 빌드해 교차 오염을 막았다. 이 저장소에는
`crypto_bench.uf2`를 만드는 빌드 디렉토리가 여러 개 있어서, **어느 이미지였는지가
기록되지 않으면 수치가 무의미해진다.** 그래서 이미지가 부팅 시 스스로 보고한다.

```
================ crypto_bench ================
  board    W6300_EVB_PICO2
  part     RP2350 (Cortex-M33)
  clk_sys  150000000 Hz
  bignum   assembly (MBEDTLS_HAVE_ASM)
  git      67fc400
  built    2026-10-14T23:51:53Z
==============================================
```

| variant | 보드 | bignum | md5 | `.text` |
|---|---|---|---|---|
| rp2040_asm | W55RP20_EVB_PICO | 어셈블리 | `1a2026d381acc24ce4deb0ffa0f78783` | 122,276 |
| rp2040_noasm | W55RP20_EVB_PICO | C | `8aded5091a694054863b419e3a2c81fa` | 122,844 |
| rp2350_asm | W6300_EVB_PICO2 | 어셈블리 | `d63400eefab7e32ee7230aa697e270ca` | 114,128 |
| rp2350_noasm | W6300_EVB_PICO2 | C | `17ff444328ecd1caa061fd940097acd7` | 115,200 |

벤치는 네트워크를 쓰지 않는다(`pico_stdlib` + `mbedcrypto` + `mbedx509`만 링크).
**이더넷 칩은 이 수치에 무관하다** — 보드 이름은 MCU와 핀 정의만 고른다.

### 클럭이 공정하지 않다 — RP2040 쪽이 유리하게 측정됐다

`CMakeLists.txt:147`이 `PICO_USE_FASTEST_SUPPORTED_CLOCK=1`을 전역으로 건다.

```
RP2040   platform_defs.h:86    → 200 MHz   (데이터시트 정격 133 MHz, 즉 오버클럭)
RP2350   platform_defs.h:115   → 150 MHz   (정격 최대, 스톡)
```

**RP2040은 정격을 1.5배 넘긴 상태로 측정됐고 RP2350은 스톡이다.** 그래도 4.1배
차이가 난다. 정격 클럭으로 환산하면 RP2040은 약 7,600 ms가 되어 격차는 6.2배다.

---

## 3. 실측값

### 3-1. OpenSecureChannel 합계 (ms)

| | RP2040 @200MHz | RP2350 @150MHz | 부품 효과 |
|---|---|---|---|
| bignum **C** | 6,973 | 2,017 | 3.46× |
| bignum **어셈블리** | 5,051 | **1,229** | 4.11× |
| 어셈블리 효과 | 1.38× | **1.64×** | |

사이클 수로 환산하면(시간 × 클럭) 부품 효과는 **5.48배**다 — 1,010M → 184M 사이클.
Cortex-M33에 32×32 곱셈 명령이 있고 M0+에는 없다는 차이가 그대로 나온다.

### 3-2. 개인키 연산 — 비용의 94%

| 연산 | RP2040 asm | RP2040 C | RP2350 asm | RP2350 C |
|---|---|---|---|---|
| RSA-2048 sign (private) | 1,647.18 | 2,273.55 | **401.64** | 658.50 |
| RSA-2048 OAEP dec (priv) | 1,544.30 | 2,127.72 | **380.84** | 620.74 |
| 첫 sign (blinding 캐시 없음) | 1,896.87 | 2,565.86 | 551.40 | 825.47 |

### 3-3. 공개키 연산

| 연산 | RP2040 asm | RP2040 C | RP2350 asm | RP2350 C |
|---|---|---|---|---|
| RSA-2048 verify (public) | 103.55 | 146.50 | **21.07** | 38.05 |
| RSA-2048 OAEP enc (pub) | 104.02 | 146.98 | **21.35** | 38.33 |

지수가 65537이라 개인키 연산보다 두 자릿수 싸다. 사양대로다.

### 3-4. 메시지마다 드는 대칭 연산 — 문제가 아니다

| 연산 | RP2040 | RP2350 |
|---|---|---|
| SHA-256 8 KB | 4.96 ms | 2.65 ms |
| AES-256-CBC 8 KB | 6.32 ms | 4.00 ms |
| HMAC-SHA256 32 B | 178.0 us | 106.2 us |
| 키유도 (14 × HMAC) | 2.98 ms | 1.68 ms |

**부담이 메시지마다가 아니라 핸드셰이크 한 번에 몰린다는 설계 전제가 실측으로
확인됐다.** 서버 버퍼(8 KB)를 가득 채운 메시지도 한 자릿수 ms다.

### 3-5. 파싱

| 연산 | RP2040 asm | RP2350 asm |
|---|---|---|
| 개인키 DER 파싱 | 213.8 us | 237.4 us |
| 인증서 파싱 | 145.3 us | 132.2 us |

시작 시 1회 / 클라이언트당 1회라 무게가 없다.

---

## 4. 합계의 구성 (RP2350 어셈블리 기준)

```
2 x OAEP decrypt (private)        762 ms     ← 62%
1 x sign response (private)       402 ms     ← 33%
2 x OAEP encrypt (public)          43 ms
1 x verify request signature       21 ms
key parse + derivation              2 ms
                              --------
                                 1229 ms
```

**개인키 연산 세 번이 95%다.** OAEP가 블록 단위라 한 OPN이 복호 2회가 되는 것이
가장 크다 (`securitypolicy_basic256sha256.c:36,131` — 평문 블록 214 B, 서명만 256 B).

---

## 5. 이 숫자가 아닌 것

**바닥값이다.** 산술만 재고 왕복·인코딩·PKI 목록 순회·DRBG 시딩·할당·이더넷 전송이
빠져 있다. 실제 클라이언트가 보는 시간은 더 길다.

**인증서 체인 검증값은 유효하지 않다.** `crypto_bench.c:360-361`이 같은 인증서를
leaf와 trust anchor로 둘 다 넣어서 mbedTLS가 즉시 통과시킨다(25~34 us). 진짜
검증은 공개키 연산 1회라 RP2350에서 약 21 ms다. **합계는 그만큼 낙관적이다**
(1,229 → 약 1,250 ms). 수정에는 CA로 서명된 인증서 한 쌍이 필요하다. **미수정.**

**RP2350에서 설명되지 않은 값 두 개.** 어셈블리와 무관해야 하는데 움직였다.

```
HMAC-SHA256 32 B         asm 106.2 us → C 135.0 us    (min 101 vs 127)
키유도 (14 × HMAC)       asm   1.68 ms → C   2.10 ms
```

SHA-256 8 KB와 AES 8 KB는 소수점까지 같고(2.65 / 4.00), RP2040에서는 HMAC도
같았다(178.0 vs 177.6). **RP2350에서만, 짧은 연산에서만** 벌어진다. 코드 배치가
바뀌어 XIP 캐시(8 KB) 적중률이 달라진 것이 후보지만 **확인하지 않았다.**
설계 판정에는 영향이 없다.

---

## 6. 설계 판정 — 2026-10-15 코드 확인 결과

### 6-1. 워치독은 존재하지 않는다

`crypto_bench`와 `current_state.md:170`이 "RSA 연산 중 워치독이 kick되지 않는다"고
서술하지만, **`examples/opcua_node`에는 워치독이 없다.** `hardware/watchdog.h`를
포함하지도, `watchdog_enable()`을 호출하지도 않는다. `opcua_settings.h:149`의
`reboot_reason`에 `1=watchdog` 값이 정의돼 있을 뿐 그 값을 쓰는 코드가 없다.

**따라서 현재 결함이 아니다.** 다만 워치독을 넣을 때 **타임아웃을 1.3초보다 충분히
길게** 잡아야 한다 — 핸드셰이크 중에는 어떤 양보 지점도 만들 수 없다(6-3 참조).

### 6-2. 동시 핸드셰이크가 남은 유일한 성능 과제

`opcua_server.c:467,494-495`가 소켓·채널·세션을 각각 4개로 둔다
(`OPCUA_MAX_SOCKETS 4`, `opcua_settings.h:43`). 이벤트루프는 한 번의 iterate에서
**소켓 4개를 순서대로 돈다**(`open62541_wiznet_eventloop.c:827,833`). 단일코어
협력 루프라 OPN 처리는 **직렬**이다.

```
1번째 클라이언트   1,229 ms
4번째 클라이언트   약 4,916 ms      ← 5,000 ms 타임아웃에 붙는다
```

설비 전원 복구로 4개가 동시에 재접속하는 상황이 현장에서 실제로 생긴다. 바닥값
기준으로 이미 여유가 없고 실제 오버헤드가 더 붙는다.

**TCP는 안 끊긴다.** W6300이 TCP를 칩 내부에서 처리하므로 정지 중에도 연결은
수립되고 OPN 바이트는 칩 RX 버퍼에 쌓인다. 잃는 것은 지연뿐이다.

선택지:

| 안 | 내용 | 남는 문제 |
|---|---|---|
| A | 보안 빌드에서 소켓 2개로 축소 | 최악 2.5초. 동시 클라이언트 2개 제한 |
| B | 4개 유지 | 동시 재접속 시 마지막 클라이언트가 타임아웃 경계 |
| C | ECC nistP256 정책 | 개인키 연산이 ms 단위로 떨어져 문제 소멸. 클라이언트 지원 범위가 좁다 |

**지금 고르지 않는다.** 1,229 ms는 바닥값이므로, L2 빌드를 올려 **실제 OPN을 한 번
재고 나서** 소켓 수를 정한다.

### 6-3. RSA 연산은 쪼갤 수 없다

벤더링된 mbedTLS 3.6.0에 `MBEDTLS_ECP_RESTARTABLE`은 있으나
(`mbedtls_config.h:839`, 주석 상태) **RSA에 대응되는 기능이 없다**(`rsa_restart`
심볼 부재). 따라서 핸드셰이크를 여러 루프 이터레이션에 나눠 양보시키는 방법이
mbedTLS 쪽에 없다. 나중에 ECC로 가면 이 기능으로 양보까지 가능해진다.

### 6-4. 엔트로피원 — RP2350으로 해결된다

**RP2350에 하드웨어 TRNG가 있다. SDK 코드로 확인됐다.**

```
libraries/pico-sdk/src/rp2350/hardware_regs/include/hardware/platform_defs.h:74
    #define HAS_RP2350_TRNG 1
libraries/pico-sdk/src/rp2_common/pico_rand/include/pico/rand.h:76,102-103
    PICO_RAND_ENTROPY_SRC_TRNG 기본 1, 시드에도 TRNG 사용
libraries/pico-sdk/src/rp2350/hardware_structs/include/hardware/structs/trng.h
```

RP2040에는 없다. **보드를 RP2350으로 정한 것이 곧 엔트로피원 결정**이고, ROSC+ADC
조합으로 품질 근거를 문서화하는 작업이 사라졌다.

**단, 아직 연결돼 있지 않다.** `mbedtls_hardware_poll`도
`MBEDTLS_ENTROPY_HARDWARE_ALT`도 저장소에 없다. L2 빌드를 올릴 때 `pico_rand`를
mbedTLS 엔트로피 소스로 등록하는 작업이 **먼저** 들어가야 한다 — 빼먹으면 CTR_DRBG가
예측 가능한 값으로 시딩된다.

---

## 7. 다음

1. **엔트로피 소스 연결** — `pico_rand` → `mbedtls_hardware_poll`. 암호화보다 먼저
2. **L2 빌드** — `-DUA_ENABLE_ENCRYPTION=MBEDTLS`. Flash/heap 증가 측정
3. **실제 OPN 1회 측정** — 바닥값 1,229 ms 대비 실제가 얼마인지. 이 값으로 6-2를 결정
4. 인증서 체인 검증 벤치 수정 (CA 서명 인증서 쌍 필요)
5. RP2350에서 HMAC이 어셈블리 설정에 따라 달라지는 이유 — 선택 사항

---

## 8. 재현 절차

```bash
cd /d/theo_git_project/WIZnet-PICO-C-CHIP-TEST
bash tools/bench_matrix.sh                       # 4개 이미지 빌드
bash tools/bench_matrix.sh --flash rp2350_asm    # BOOTSEL 보드로 복사
# 또는 실행 중인 보드를 호스트에서 BOOTSEL로 넣기:
bash tools/bench_matrix.sh --flash rp2350_asm COM12
```

`build_bench_out/manifest.txt`에 variant · 보드 · bignum · md5 · 시각이 남는다.
시리얼 출력의 배너가 의도한 부품·설정과 다르면 **그 로그는 버린다.**

보드 두 개가 필요하다: `W55RP20_EVB_PICO`(RP2040), `W6300_EVB_PICO2`(RP2350).
