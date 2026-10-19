#!/usr/bin/env python3
"""Socket and session reclaim checks for the OPC UA node (F-6).

    python tools/opcua_reclaim_test.py idle     [url]
    python tools/opcua_reclaim_test.py orphans  [url]
    python tools/opcua_reclaim_test.py hold     [url]

idle
    Opens a TCP connection to the server and sends nothing. open62541 gives a
    new channel 10 s to finish its handshake and then closes it; this guards
    that behaviour, since the transport relies on it instead of a timer of its
    own (verified before and after the F-6 change: closed at 10.0 s).

orphans
    Starts four clients in child processes, lets each create a session, then
    kills the processes. The OS closes their TCP connections but nobody sends
    CloseSession, so four sessions are left behind until maxSessionTimeout.
    With sessions == sockets the next client got BadTooManySessions for ten
    minutes; now two more clients must still connect.

hold
    Opens a session and then stays completely silent (no reads, client
    watchdog off) for three minutes. Pull the PC's network cable while it
    holds: the board can no longer reach the client, nothing sends a FIN, and
    the board's console should print "dropped by chip" once keepalive gives
    up. That line's "peer silent" value is the reclaim time.

All use SecurityPolicy#None so the result does not depend on handshake cost.
"""

import asyncio
import multiprocessing
import socket
import sys
import time
from urllib.parse import urlparse

from asyncua import Client

DEFAULT_URL = "opc.tcp://192.168.11.2:4840"
FIRST_BYTE_MS = 10_000


def idle(url: str) -> int:
    u = urlparse(url)
    s = socket.create_connection((u.hostname, u.port or 4840), timeout=5)
    s.settimeout(FIRST_BYTE_MS / 1000 + 10)
    started = time.perf_counter()
    try:
        data = s.recv(1)
        how = "closed (FIN)" if data == b"" else f"sent {data!r}"
    except ConnectionResetError:
        how = "reset (RST)"
    except socket.timeout:
        how = "still open"
    elapsed = time.perf_counter() - started
    s.close()
    print(f"silent connection: {how} after {elapsed:.1f} s "
          f"(expect closed at ~{FIRST_BYTE_MS / 1000:.0f} s)")
    ok = how != "still open" and abs(elapsed - FIRST_BYTE_MS / 1000) < 3
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


def _hold_session(url: str, ready) -> None:
    async def run():
        c = Client(url=url, timeout=10)
        await c.connect()
        ready.set()
        await asyncio.sleep(3600)
    asyncio.run(run())


async def _connect_once(url: str) -> str:
    c = Client(url=url, timeout=10)
    try:
        await c.connect()
        await c.disconnect()
        return "ok"
    except Exception as exc:                        # noqa: BLE001 - reporting tool
        return f"{type(exc).__name__}: {exc}"


def orphans(url: str) -> int:
    ctx = multiprocessing.get_context("spawn")
    procs = []
    for i in range(4):
        ev = ctx.Event()
        p = ctx.Process(target=_hold_session, args=(url, ev), daemon=True)
        p.start()
        if not ev.wait(20):
            print(f"client {i} did not connect")
            return 1
        procs.append(p)
    print("4 sessions open; killing the clients without CloseSession")
    for p in procs:
        p.kill()
        p.join()
    time.sleep(2)                                   # let the sockets close

    results = [asyncio.run(_connect_once(url)) for _ in range(2)]
    for i, r in enumerate(results):
        print(f"new client {i}: {r}")
    ok = all(r == "ok" for r in results)
    print("PASS" if ok else "FAIL (orphan sessions still block new clients)")
    return 0 if ok else 1


def hold(url: str, seconds: float = 180.0) -> int:
    async def run():
        c = Client(url=url, timeout=10, watchdog_intervall=3600.0)
        await c.connect()
        t0 = time.strftime("%H:%M:%S")
        print(f"{t0} session open and silent for {seconds:.0f} s - "
              f"pull the PC network cable now, keep it out ~60 s")
        await asyncio.sleep(seconds)
        print(time.strftime("%H:%M:%S"), "done (the session may already be gone)")
    try:
        asyncio.run(run())
    except Exception as exc:                        # noqa: BLE001 - reporting tool
        print(f"{type(exc).__name__}: {exc}")
    return 0


if __name__ == "__main__":
    modes = {"idle": idle, "orphans": orphans, "hold": hold}
    if len(sys.argv) < 2 or sys.argv[1] not in modes:
        print(__doc__)
        sys.exit(2)
    target = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_URL
    sys.exit(modes[sys.argv[1]](target))
