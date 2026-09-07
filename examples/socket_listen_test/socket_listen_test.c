/*
 * socket_listen_test — W6300 소켓 검증용 단발성 시험 코드
 *
 * 시험 1 (F-1) : 여러 하드웨어 소켓이 같은 TCP 포트로 동시에 LISTEN 되는가
 *                → 2026-09-03 PASS (4KB·6KB 배분 양쪽에서 확인)
 *
 * 시험 2 (D-5) : Sn_TX_BSR / Sn_RX_BSR 에 2의 거듭제곱이 아닌 6KB 를 넣어도
 *                버퍼 read/write 가 정상 동작하는가
 *
 *   데이터시트 DS_V102E p.68 은 0/1/2/4/8/16/32 만 유효하다고 명시하고,
 *   그 외 값은 "causes a malfunction in buffer read/write access process"
 *   라고 경고한다. 6 은 목록에 없다.
 *   ioLibrary wizchip_init() 은 총합 32 초과만 검사하므로 실패가 조용하다.
 *
 *   판정: 매직으로 시작하는 카운터 패턴 64KB 를 흘려 넣어 wraparound 를
 *         여러 번 강제하고, 수신 순서가 그대로인지 장치가 직접 검사한다.
 *
 *   매직이 없는 접속(OPC UA 트래픽 등)은 에코만 하고 판정하지 않는다.
 *   1차 시도에서 UAExpert 트래픽을 "깨짐"으로 오판한 적이 있어 넣은 장치다.
 *
 * 이 파일은 시험용이며 제품 코드가 아니다. opcua_node 는 건드리지 않는다.
 */

#include <stdio.h>
#include <string.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"

#define TEST_SOCKET_COUNT 4u
/* 4840 은 UAExpert 가 계속 재접속을 시도해 시험을 오염시킨다. */
#define TEST_PORT 5000u
#define RX_BUF_SIZE 1024u

/* 처리량 하한. 이 밑이면 FAIL 로 본다.
 *
 * 왜 필요한가: 6 KB 버퍼를 시험했을 때 바이트는 하나도 안 틀렸는데
 * Sn_RX_RSR 이 1 로 고정되어 폴링당 1바이트씩만 읽혔다 (약 800 B/s).
 * 무결성만 보던 1차 판정이 이것을 통과시켰고, OPC UA 는 타임아웃으로 무너졌다.
 * 정상 배분에서는 수백 KB/s 가 나오므로 10 KB/s 는 넉넉한 하한이다. */
#define MIN_THROUGHPUT_BPS 10000u

/* 소켓 예산. 2의 거듭제곱만 쓴다.
 * TX 합계 = 4*4 + 8 + 2 + 2 + 0 = 28KB, RX 동일.
 * 6 KB 를 넣으면 Sn_RX_RSR 이 1 로 고정되어 처리량이 무너진다
 * (2026-09-03 실기 확인). */
static uint8_t g_memsize[2][8] = {
    {4, 4, 4, 4, 8, 2, 2, 0},
    {4, 4, 4, 4, 8, 2, 2, 0}
};

/* 시험 데이터임을 알리는 매직. 이게 앞에 붙어야 판정을 낸다. */
static const uint8_t MAGIC[4] = {0xA5, 0x5A, 0xA5, 0x5A};

static wiz_NetInfo g_net_info = {
    .mac  = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .ip   = {192, 168, 11, 2},
    .sn   = {255, 255, 255, 0},
    .gw   = {192, 168, 11, 1},
    .dns  = {8, 8, 8, 8},
    .dhcp = NETINFO_STATIC
};

static uint8_t  g_rx[RX_BUF_SIZE];
static uint8_t  g_prev_sr[TEST_SOCKET_COUNT];
static uint32_t g_conn_count[TEST_SOCKET_COUNT];

static uint8_t  g_expect[TEST_SOCKET_COUNT];
static bool     g_synced[TEST_SOCKET_COUNT];
static uint32_t g_total[TEST_SOCKET_COUNT];
static uint32_t g_errors[TEST_SOCKET_COUNT];
static uint32_t g_first_err[TEST_SOCKET_COUNT];
static uint8_t  g_magic_seen[TEST_SOCKET_COUNT];
static bool     g_verify[TEST_SOCKET_COUNT];
static uint32_t g_start_ms[TEST_SOCKET_COUNT];   /* 매직을 본 시각 */

static const char *sr_name(uint8_t sr) {
    switch(sr) {
    case SOCK_CLOSED:      return "CLOSED";
    case SOCK_INIT:        return "INIT";
    case SOCK_LISTEN:      return "LISTEN";
    case SOCK_ESTABLISHED: return "ESTABLISHED";
    case SOCK_CLOSE_WAIT:  return "CLOSE_WAIT";
    case SOCK_SYNRECV:     return "SYNRECV";
    case SOCK_SYNSENT:     return "SYNSENT";
    case SOCK_FIN_WAIT:    return "FIN_WAIT";
    case SOCK_TIME_WAIT:   return "TIME_WAIT";
    case SOCK_LAST_ACK:    return "LAST_ACK";
    default:               return "?";
    }
}

static void reset_check(uint8_t sn) {
    g_expect[sn]     = 0;
    g_synced[sn]     = false;
    g_total[sn]      = 0;
    g_errors[sn]     = 0;
    g_first_err[sn]  = 0;
    g_magic_seen[sn] = 0;
    g_verify[sn]     = false;
    g_start_ms[sn]   = 0;
}

static void check_pattern(uint8_t sn, const uint8_t *buf, int32_t len) {
    int32_t i = 0;

    /* 매직을 먼저 찾는다. 못 찾으면 판정 대상이 아니다. */
    while(i < len && !g_verify[sn]) {
        if(buf[i] == MAGIC[g_magic_seen[sn]]) {
            g_magic_seen[sn]++;
            if(g_magic_seen[sn] == sizeof(MAGIC)) {
                g_verify[sn] = true;
                g_synced[sn] = false;
                g_total[sn]  = 0;
                g_start_ms[sn] = to_ms_since_boot(get_absolute_time());
                printf("  [sock %u] magic found - pattern check ON\r\n", sn);
            }
        } else {
            g_magic_seen[sn] = (buf[i] == MAGIC[0]) ? 1u : 0u;
        }
        i++;
    }

    if(!g_verify[sn]) return;

    for(; i < len; i++) {
        if(!g_synced[sn]) {
            g_expect[sn] = buf[i];
            g_synced[sn] = true;
        } else if(buf[i] != g_expect[sn]) {
            if(g_errors[sn] == 0) {
                g_first_err[sn] = g_total[sn];
                printf("  !! [sock %u] MISMATCH at byte %lu : got 0x%02X expected 0x%02X\r\n",
                       sn, (unsigned long)g_first_err[sn], buf[i], g_expect[sn]);
            }
            g_errors[sn]++;
            g_expect[sn] = buf[i];
        }
        g_expect[sn]++;
        g_total[sn]++;
    }
}

static bool open_and_listen(uint8_t sn) {
    if(socket(sn, Sn_MR_TCP, TEST_PORT, SF_TCP_NODELAY) != (int8_t)sn) {
        printf("  [sock %u] socket() FAILED\r\n", sn);
        return false;
    }
    if(getSn_SR(sn) != SOCK_INIT) {
        printf("  [sock %u] SR=%s (expected INIT)\r\n", sn, sr_name(getSn_SR(sn)));
        return false;
    }
    if(listen(sn) != SOCK_OK) {
        printf("  [sock %u] listen() FAILED\r\n", sn);
        return false;
    }
    for(int i = 0; i < 100; i++) {
        if(getSn_SR(sn) == SOCK_LISTEN) break;
        sleep_ms(1);
    }
    uint8_t sr = getSn_SR(sn);
    printf("  [sock %u] SR=%-11s Sn_PORTR=%u  %s\r\n",
           sn, sr_name(sr), getSn_PORTR(sn),
           (sr == SOCK_LISTEN) ? "OK" : "<-- NOT LISTENING");
    return sr == SOCK_LISTEN;
}

int main(void) {
    stdio_init_all();
    sleep_ms(3000);
    for(uint32_t i = 0; i < 50u && !stdio_usb_connected(); i++)
        sleep_ms(100);

    printf("\r\n==========================================================\r\n");
    printf(" W6300 SOCKET TEST   build %s %s\r\n", __DATE__, __TIME__);
    printf("   test 1 (F-1) : multi-listen on one port\r\n");
    printf("   test 2 (D-5) : non-power-of-2 buffer size (6KB)\r\n");
    printf("==========================================================\r\n");

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();

    /* QSPI 콜백 등록 + 기본 배분 {4,4,...}. 이것을 먼저 돌려야
     * 레지스터 접근이 성립한다. 건너뛰면 모든 읽기가 0x00 이 된다. */
    wizchip_initialize();

    printf("\r\n--- applying memsize (TX/RX identical) ---\r\n");
    printf("  default   :");
    for(int i = 0; i < 8; i++) printf(" %u", getSn_TXBUF_SIZE(i));
    printf("   (from wizchip_initialize)\r\n");

    printf("  requested :");
    int sum = 0;
    for(int i = 0; i < 8; i++) { printf(" %u", g_memsize[0][i]); sum += g_memsize[0][i]; }
    printf("   (sum %d KB)\r\n", sum);

    int8_t rc = ctlwizchip(CW_INIT_WIZCHIP, (void *)g_memsize);
    printf("  ctlwizchip(CW_INIT_WIZCHIP) -> %d %s\r\n",
           rc, (rc == 0) ? "(accepted)" : "(REJECTED)");

    printf("  readback  :");
    bool readback_ok = true;
    for(int i = 0; i < 8; i++) {
        uint8_t tx = getSn_TXBUF_SIZE(i);
        printf(" %u", tx);
        if(tx != g_memsize[0][i]) readback_ok = false;
    }
    printf("   %s\r\n", readback_ok ? "MATCH" : "<-- DIFFERS from requested");

    wizchip_check();
    network_initialize(g_net_info);
    print_network_information(g_net_info);

    printf("\r\n--- opening %u sockets on port %u ---\r\n",
           TEST_SOCKET_COUNT, TEST_PORT);
    uint32_t listening = 0;
    for(uint8_t sn = 0; sn < TEST_SOCKET_COUNT; sn++) {
        reset_check(sn);
        if(open_and_listen(sn)) listening++;
    }
    printf("  %lu / %u reached SOCK_LISTEN  ==> %s\r\n",
           (unsigned long)listening, TEST_SOCKET_COUNT,
           (listening == TEST_SOCKET_COUNT) ? "test 1 PASS" : "test 1 FAIL");

    printf("\r\n----------------------------------------------------------\r\n");
    printf("TEST 2 - push a counter pattern through a 6KB buffer.\r\n");
    printf("Data MUST start with magic A5 5A A5 5A, otherwise it is ignored.\r\n");
    printf("PowerShell:\r\n");
    printf("  $m=[byte[]](0xA5,0x5A,0xA5,0x5A)\r\n");
    printf("  $d=[byte[]]::new(65536)\r\n");
    printf("  for($i=0;$i -lt 65536;$i++){$d[$i]=$i %% 256}\r\n");
    printf("  [IO.File]::WriteAllBytes(\"$PWD\\tx.bin\", $m + $d)\r\n");
    printf("  ncat 192.168.11.2 %u < tx.bin > rx.bin\r\n", TEST_PORT);
    printf("  fc /b tx.bin rx.bin\r\n");
    printf("64KB through a 6KB buffer forces ~11 wraparounds.\r\n");
    printf("----------------------------------------------------------\r\n\r\n");

    for(uint8_t sn = 0; sn < TEST_SOCKET_COUNT; sn++)
        g_prev_sr[sn] = getSn_SR(sn);

    uint32_t last_report = 0;

    while(1) {
        for(uint8_t sn = 0; sn < TEST_SOCKET_COUNT; sn++) {
            uint8_t sr = getSn_SR(sn);

            if(sr != g_prev_sr[sn]) {
                printf("[sock %u] %s -> %s", sn, sr_name(g_prev_sr[sn]), sr_name(sr));
                if(sr == SOCK_ESTABLISHED) {
                    uint8_t ip[4];
                    getSn_DIPR(sn, ip);
                    g_conn_count[sn]++;
                    reset_check(sn);
                    printf("   peer=%u.%u.%u.%u:%u (conn #%lu)",
                           ip[0], ip[1], ip[2], ip[3], getSn_DPORTR(sn),
                           (unsigned long)g_conn_count[sn]);
                }
                printf("\r\n");

                if(g_prev_sr[sn] == SOCK_ESTABLISHED) {
                    if(!g_verify[sn]) {
                        printf("  -- [sock %u] no magic - not a pattern test, ignored\r\n", sn);
                    } else {
                        uint32_t ms = to_ms_since_boot(get_absolute_time())
                                      - g_start_ms[sn];
                        if(ms == 0u) ms = 1u;
                        uint32_t bps = (uint32_t)(((uint64_t)g_total[sn] * 1000u) / ms);

                        bool ok_data = (g_errors[sn] == 0u);
                        bool ok_rate = (bps >= MIN_THROUGHPUT_BPS);

                        printf("  == [sock %u] RX %lu B in %lu ms = %lu B/s\r\n",
                               sn, (unsigned long)g_total[sn],
                               (unsigned long)ms, (unsigned long)bps);
                        printf("     integrity : %s (%lu mismatches)\r\n",
                               ok_data ? "PASS" : "FAIL",
                               (unsigned long)g_errors[sn]);
                        printf("     throughput: %s (min %u B/s)\r\n",
                               ok_rate ? "PASS" : "FAIL", MIN_THROUGHPUT_BPS);
                        printf("     ==> %s\r\n",
                               (ok_data && ok_rate) ? "PASS" : "FAIL");
                        if(g_errors[sn])
                            printf("     first mismatch at byte %lu\r\n",
                                   (unsigned long)g_first_err[sn]);
                        if(!ok_rate)
                            printf("     NOTE: check Sn_RX_RSR - a non-power-of-2 "
                                   "buffer size makes it report 1\r\n");
                    }
                }
                g_prev_sr[sn] = sr;
            }

            if(sr == SOCK_ESTABLISHED) {
                uint16_t avail = getSn_RX_RSR(sn);
                if(avail > 0u) {
                    if(avail > RX_BUF_SIZE) avail = RX_BUF_SIZE;
                    int32_t n = recv(sn, g_rx, avail);
                    if(n > 0) {
                        check_pattern(sn, g_rx, n);
                        int32_t off = 0;
                        while(off < n) {
                            int32_t s = send(sn, g_rx + off, (uint16_t)(n - off));
                            if(s <= 0) break;
                            off += s;
                        }
                    }
                }
            } else if(sr == SOCK_CLOSE_WAIT) {
                disconnect(sn);
            } else if(sr == SOCK_CLOSED) {
                if(socket(sn, Sn_MR_TCP, TEST_PORT, SF_TCP_NODELAY) == (int8_t)sn)
                    listen(sn);
            }
        }

        uint32_t now = to_ms_since_boot(get_absolute_time());
        if(now - last_report >= 10000u) {
            last_report = now;
            printf("[state]");
            for(uint8_t sn = 0; sn < TEST_SOCKET_COUNT; sn++) {
                printf("  s%u=%s", sn, sr_name(getSn_SR(sn)));
                if(g_verify[sn])
                    printf("(%luB,%luerr)", (unsigned long)g_total[sn],
                           (unsigned long)g_errors[sn]);
            }
            printf("\r\n");
        }

        sleep_ms(1);
    }
}
