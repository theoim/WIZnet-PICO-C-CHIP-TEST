# -*- coding: utf-8 -*-
"""A-5 soak — 누수 원인을 두 가지로 분리해서 본다.

  (a) 시간에 따른 누수   : 가만히 둔 세션이 시간이 지나며 힙을 먹는가
  (b) 세션 개폐당 누수   : 접속/해제를 반복하면 회당 힙이 줄어드는가

구성:
  - STEADY 세션 2개를 계속 열어둔다 (구독 + 모니터 항목 각 1개)
  - CHURN 세션 1개를 CYCLE_S 마다 열었다 닫는다
  - FreeHeap 을 두 지점에서 읽는다
        idle  = churn 세션이 닫혀 있을 때  -> (b) 판정용, 조건이 항상 동일
        peak  = churn 세션이 열려 있을 때  -> 최대 사용량 추적

UAExpert 가 붙어 있으면 소켓 하나를 더 쓰므로 최대 4개가 된다.
그대로 두어도 된다 — 오히려 실사용에 가깝다.

출력: CSV 한 줄씩 + 30분마다 추세 요약.
중단은 Ctrl+C. 중단해도 요약을 찍고 끝난다.
"""
import asyncio, csv, os, sys, time, logging
from datetime import datetime
from asyncua import Client

URL      = os.environ.get("SOAK_URL", "opc.tcp://192.168.11.2:4840")
HEAP     = "ns=2;s=Diag.FreeHeap"
UPTIME   = "ns=2;s=Diag.Uptime_s"
REBOOTS  = "ns=2;s=Diag.RebootCount"
CH1      = "ns=2;s=Channels/Channel_1"

STEADY_N  = 2
CYCLE_S   = float(os.environ.get("SOAK_CYCLE_S", "60"))   # churn 주기
HOURS     = float(os.environ.get("SOAK_HOURS", "12"))
CSV_PATH  = os.environ.get("SOAK_CSV", "soak_log.csv")
REPORT_S  = float(os.environ.get("SOAK_REPORT_S", "1800"))  # 요약 주기
OP_TIMEOUT = 20.0    # 개별 읽기 상한

# 상한이 왜 필요한가: 2026-09-07 실행에서 이게 없어서 세션이 반쯤 죽었을 때
# read_value() 가 영원히 기다렸다. 프로세스는 살아 있는데 1시간 42분 동안
# 아무것도 기록하지 않았고, 로그만 보면 조용히 잘 도는 것처럼 보였다.
# 멈춘 시험은 실패한 시험보다 나쁘다 - 실패는 눈에 띄기라도 한다.

logging.getLogger("asyncua").setLevel(logging.CRITICAL)


class Sink:
    def __init__(self): self.n = 0
    def datachange_notification(self, node, val, data): self.n += 1


async def connect(subscribe=True):
    c = Client(url=URL, timeout=10)
    c.session_timeout = 600000
    await c.connect()
    sink = None
    if subscribe:
        sink = Sink()
        sub = await c.create_subscription(500, sink)
        await sub.subscribe_data_change(c.get_node(CH1))
    return c, sink


async def sample(cli):
    """읽기 지연도 같이 잰다 — 시간이 지나며 느려지는지 본다."""
    t0 = time.monotonic()
    heap = await asyncio.wait_for(cli.get_node(HEAP).read_value(), OP_TIMEOUT)
    lat  = (time.monotonic() - t0) * 1000.0
    up   = await asyncio.wait_for(cli.get_node(UPTIME).read_value(), OP_TIMEOUT)
    rb   = await asyncio.wait_for(cli.get_node(REBOOTS).read_value(), OP_TIMEOUT)
    return heap, up, rb, lat


def trend(pts):
    """(t, heap) 목록에서 시간당 기울기(B/h)를 최소자승으로 구한다."""
    if len(pts) < 3: return None
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    den = sum((p[0]-mx)**2 for p in pts)
    if den == 0: return None
    num = sum((p[0]-mx)*(p[1]-my) for p in pts)
    return (num/den) * 3600.0


async def main():
    print(f"soak start  url={URL}  steady={STEADY_N}  cycle={CYCLE_S:.0f}s  "
          f"limit={HOURS}h  csv={CSV_PATH}", flush=True)

    steady, sinks = [], []
    for i in range(STEADY_N):
        c, s = await connect()
        steady.append(c); sinks.append(s)
        print(f"  steady session {i+1} up", flush=True)

    f = open(CSV_PATH, "w", newline="", encoding="utf-8")
    w = csv.writer(f)
    w.writerow(["wall", "elapsed_s", "cycle", "phase", "free_heap",
                "uptime_s", "reboots", "read_ms", "notif", "errors"])
    f.flush()

    t_start = time.monotonic()
    t_report = t_start
    cycle = 0
    errors = 0
    base_uptime = None
    idle_pts, peak_pts = [], []

    try:
        while (time.monotonic() - t_start) < HOURS * 3600.0:
            cycle += 1
            row_t = time.monotonic() - t_start

            # ── idle 표본: churn 세션이 닫혀 있는 상태 ──
            try:
                heap, up, rb, lat = await sample(steady[0])
                if base_uptime is None: base_uptime = up
                if rb != 0:
                    print(f"!! RebootCount={rb} - 장비가 재부팅했다", flush=True)
                idle_pts.append((row_t, heap))
                notif = sum(s.n for s in sinks)
                w.writerow([datetime.now().isoformat(timespec="seconds"),
                            f"{row_t:.1f}", cycle, "idle", heap, up, rb,
                            f"{lat:.1f}", notif, errors])
                f.flush()
            except Exception as e:
                errors += 1
                print(f"[{cycle}] idle sample FAIL: {type(e).__name__}: {e}", flush=True)

            # ── churn: 열고 읽고 닫는다 ──
            churn = None
            try:
                churn, _ = await connect()
                await asyncio.sleep(2.0)
                heap, up, rb, lat = await sample(steady[0])
                peak_pts.append((row_t, heap))
                w.writerow([datetime.now().isoformat(timespec="seconds"),
                            f"{time.monotonic()-t_start:.1f}", cycle, "peak",
                            heap, up, rb, f"{lat:.1f}",
                            sum(s.n for s in sinks), errors])
                f.flush()
            except Exception as e:
                errors += 1
                print(f"[{cycle}] churn connect FAIL: {type(e).__name__}: {e}", flush=True)
            finally:
                if churn is not None:
                    try: await churn.disconnect()
                    except Exception: errors += 1

            # ── 주기 요약 ──
            if time.monotonic() - t_report >= REPORT_S:
                t_report = time.monotonic()
                h  = (time.monotonic() - t_start) / 3600.0
                # 관측 창이 짧으면 기울기가 무의미하다 - 10분 넘어야 찍는다
                si = trend(idle_pts) if (time.monotonic()-t_start) > 600 else None
                d  = idle_pts[0][1] - idle_pts[-1][1] if len(idle_pts) >= 2 else 0
                print(f"-- {h:5.2f}h  cycles={cycle}  idle_heap={idle_pts[-1][1]:,}"
                      f"  drop={d:+,}B"
                      f"  trend={si:+.1f} B/h" if si is not None else
                      f"-- {h:5.2f}h  cycles={cycle}", flush=True)

            await asyncio.sleep(max(0.0, CYCLE_S - 2.0))

    except (KeyboardInterrupt, asyncio.CancelledError):
        print("\n중단됨 - 요약을 찍는다", flush=True)
    finally:
        for c in steady:
            try: await c.disconnect()
            except Exception: pass
        f.close()

    # ── 최종 판정 ──
    print("\n================ soak 결과 ================", flush=True)
    hrs = (time.monotonic() - t_start) / 3600.0
    print(f"기간        : {hrs:.2f} h")
    print(f"churn 회수  : {cycle}  (세션 개폐 {cycle}회)")
    print(f"오류        : {errors}")
    if len(idle_pts) >= 2:
        first, last = idle_pts[0][1], idle_pts[-1][1]
        drop = first - last
        si = trend(idle_pts)
        print(f"idle FreeHeap : {first:,} -> {last:,}   ({-drop:+,} B)")
        if si is not None:
            print(f"추세          : {si:+.1f} B/h")
        if cycle:
            print(f"개폐 1회당    : {drop/cycle:+.1f} B")
        print()
        if abs(drop) < 512:
            print("판정: 누수 없음 (변동 512 B 미만 = 단편화 수준)")
        elif drop > 0:
            print(f"판정: 감소 추세. 개폐당 {drop/cycle:.1f} B - 원인 추적 필요")
        else:
            print("판정: 힙이 오히려 늘었다 - 지연 회수. 정상")
    print(f"\nCSV: {os.path.abspath(CSV_PATH)}")

asyncio.run(main())
