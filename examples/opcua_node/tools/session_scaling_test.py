# -*- coding: utf-8 -*-
"""OPC UA 세션 확장 시험.

목적 두 가지:
  1) 세션 1~4개일 때 FreeHeap 실측  (memory_budget.md 표의 3·4행을 실측으로 대체)
  2) 5번째 접속이 어떻게 거절되는가  (product_direction.md 8-5)

조건을 기존 표와 맞춘다: 세션마다 Subscription 1개 + MonitoredItem 1개.
UAExpert 가 하던 것과 같은 부하다.
"""
import asyncio, sys, time
from asyncua import Client, ua

URL      = "opc.tcp://192.168.11.2:4840"
HEAP     = "ns=2;s=Diag.FreeHeap"
N_TRY    = 5           # 4개까지 성공 기대, 5번째가 관심사
SETTLE   = 2.0         # 접속 후 안정화 대기 (s)
T_CONN   = 10.0        # 접속 시도 타임아웃 (s)


class Sink:
    def __init__(self, tag): self.tag = tag; self.n = 0
    def datachange_notification(self, node, val, data): self.n += 1


async def read_heap(cli):
    return await cli.get_node(HEAP).read_value()


async def main():
    clients = []
    rows    = []          # (세션수, FreeHeap)
    verdict = None

    for i in range(1, N_TRY + 1):
        cli = Client(url=URL, timeout=T_CONN)
        cli.session_timeout = 600000
        t0 = time.monotonic()
        try:
            await asyncio.wait_for(cli.connect(), timeout=T_CONN)
        except Exception as e:
            dt = time.monotonic() - t0
            print(f"[{i}] CONNECT FAIL  {dt:6.2f}s  {type(e).__name__}: {e}")
            verdict = (i, dt, type(e).__name__, str(e))
            try: await cli.disconnect()
            except Exception: pass
            break

        dt = time.monotonic() - t0
        print(f"[{i}] connected      {dt:6.2f}s")
        clients.append(cli)

        # 세션마다 구독 1개 + 모니터 항목 1개
        sub = await cli.create_subscription(500, Sink(i))
        await sub.subscribe_data_change(cli.get_node(HEAP))

        await asyncio.sleep(SETTLE)
        # 읽기는 항상 1번 세션으로 — 읽는 주체를 고정한다
        heap = await read_heap(clients[0])
        rows.append((i, heap))
        print(f"     sessions={i}  FreeHeap={heap:,}")

    # ── 5번째가 막혔다면: 하나 닫고 자리가 나는지 확인 ──
    reuse = None
    if verdict and len(clients) >= 2:
        print("\n-- 세션 1개 닫고 재시도 (소켓이 LISTEN 으로 돌아오는가) --")
        await clients[-1].disconnect()
        clients.pop()
        await asyncio.sleep(2.0)
        cli = Client(url=URL, timeout=T_CONN)
        t0 = time.monotonic()
        try:
            await asyncio.wait_for(cli.connect(), timeout=T_CONN)
            dt = time.monotonic() - t0
            print(f"     재접속 성공  {dt:6.2f}s  ==> 소켓 회수 정상")
            reuse = ("PASS", dt)
            clients.append(cli)
        except Exception as e:
            dt = time.monotonic() - t0
            print(f"     재접속 실패  {dt:6.2f}s  {type(e).__name__}: {e}")
            reuse = ("FAIL", dt)

    for c in clients:
        try: await c.disconnect()
        except Exception: pass

    # ── 요약 ──
    print("\n================ 결과 ================")
    print("| sessions | FreeHeap | delta |")
    print("|---------:|---------:|------:|")
    prev = None
    for n, h in rows:
        d = "" if prev is None else f"{h - prev:+,}"
        print(f"| {n} | {h:,} | {d} |")
        prev = h
    if len(rows) >= 2:
        step = (rows[0][1] - rows[-1][1]) / (rows[-1][0] - rows[0][0])
        print(f"\n세션당 평균 증가분: {step:,.0f} B")
    if verdict:
        i, dt, et, msg = verdict
        print(f"\n{i}번째 접속: 거절됨 ({dt:.2f}s, {et})")
        print(f"  {msg}")
        print(f"  판정: {'타임아웃 (칩이 무응답)' if dt >= T_CONN - 0.5 else '즉시 거절 (RST)'}")
    else:
        print(f"\n{N_TRY}개 전부 접속됨 — 상한에 도달하지 못했다")
    if reuse:
        print(f"소켓 회수: {reuse[0]} ({reuse[1]:.2f}s)")

asyncio.run(main())
