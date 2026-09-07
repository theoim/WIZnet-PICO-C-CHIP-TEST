# -*- coding: utf-8 -*-
"""소켓이 꽉 찼을 때 추가 접속이 어떻게 거절되는가.

UAExpert 가 이미 소켓 1개를 쓰고 있다고 가정하고, 스크립트가 3개를 더 연다.
합계 4개 = OPCUA_MAX_SOCKETS. 그 상태에서 원시 TCP 연결을 시도한다.
"""
import asyncio, socket, time
from asyncua import Client

IP, PORT = "192.168.11.2", 4840
URL  = f"opc.tcp://{IP}:{PORT}"
HEAP = "ns=2;s=Diag.FreeHeap"

class Sink:
    def datachange_notification(self, n, v, d): pass

def raw_try(tag, timeout=8.0):
    """OPC UA 계층 없이 TCP 3-way handshake 만 시도한다."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    t0 = time.monotonic()
    try:
        s.connect((IP, PORT))
        dt = time.monotonic() - t0
        print(f"  {tag}: ESTABLISHED  {dt*1000:7.1f} ms")
        s.close()
        return ("open", dt)
    except socket.timeout:
        dt = time.monotonic() - t0
        print(f"  {tag}: TIMEOUT      {dt*1000:7.1f} ms  (무응답 - SYN 이 버려짐)")
        return ("timeout", dt)
    except ConnectionRefusedError:
        dt = time.monotonic() - t0
        print(f"  {tag}: REFUSED      {dt*1000:7.1f} ms  (RST 수신)")
        return ("refused", dt)
    except OSError as e:
        dt = time.monotonic() - t0
        print(f"  {tag}: OSError      {dt*1000:7.1f} ms  {e}")
        return ("oserror", dt)
    finally:
        try: s.close()
        except Exception: pass

async def main():
    print("== 0. 빈 상태에서 원시 TCP (기준) ==")
    raw_try("baseline")

    print("\n== 1. 스크립트 세션 3개 연결 (UAExpert 포함 합계 4개) ==")
    cl = []
    for i in range(1, 4):
        c = Client(url=URL, timeout=10); c.session_timeout = 600000
        await c.connect()
        sub = await c.create_subscription(500, Sink())
        await sub.subscribe_data_change(c.get_node(HEAP))
        cl.append(c)
        await asyncio.sleep(1.5)
        h = await cl[0].get_node(HEAP).read_value()
        print(f"  script session {i} up   FreeHeap={h:,}")

    await asyncio.sleep(1.0)
    print("\n== 2. 소켓 포화 상태에서 원시 TCP 3회 ==")
    res = [raw_try(f"attempt {k}") for k in range(1, 4)]

    print("\n== 3. 세션 1개 닫고 즉시 재시도 ==")
    await cl.pop().disconnect()
    await asyncio.sleep(1.5)
    raw_try("after close")

    for c in cl:
        try: await c.disconnect()
        except Exception: pass

    print("\n---- 판정 ----")
    kinds = {r[0] for r in res}
    if kinds == {"refused"}:
        print("소켓 포화 시 동작: 명시적 거절 (RST). 클라이언트가 매달리지 않는다.")
    elif kinds == {"timeout"}:
        print("소켓 포화 시 동작: 무응답. 클라이언트가 타임아웃까지 대기한다.")
    else:
        print(f"일관되지 않음: {kinds} - 재현 필요")

asyncio.run(main())
