#!/usr/bin/env python3
"""Drive the OPC UA update path from the UART and check what clients see.

    PC --serial--> USB-TTL --> RP2350 UART0 (GP1 RX) --> data table
       --> address space --> subscription --> asyncua client(s)

    python tools/opcua_uart_feed_test.py --port COM4 [--rate 10] [--seconds 60]
           [--clients 1] [--secure] [--lifetime 0] [--url opc.tcp://...]

Every frame carries its own sequence number so a value seen by a client can be
traced to the frame that produced it:

    $DATA:<seq>,<seq mod 1000 + 0.5>,<2*seq + 1>

Measured separately, because they answer different questions:

  integrity      a one-call Read of all three channels must satisfy
                 ch3 == 2*ch1 + 1 - the three values came from one frame.
  latest value   after sending stops, every client must read the last seq.
  delivery       distinct seqs seen by notification / frames sent. A
                 subscription samples, it does not queue every write, so this
                 is below 100 % by design whenever frames arrive faster than
                 the sampling interval. It is reported, not judged.
  latency        notification arrival minus the send time of that seq.
  board count    Diag.UartFrameCount delta against frames sent: every frame
                 the board parsed, independent of sampling.

--lifetime N asks for an N ms SecureChannel lifetime so renewals happen during
the run (75 % of N). --clients connects them two seconds apart: simultaneous
RSA handshakes beyond two are refused by design.
"""

import argparse
import asyncio
import statistics
import threading
import time

import serial
from asyncua import Client, ua

from opcua_concurrent_test import APP_URI, CERT, DEFAULT_URL, KEY, POLICY

NS = 2
CH = [f"ns={NS};s=Channels/Channel_{i}" for i in (1, 2, 3)]
UART_FRAMES = f"ns={NS};s=Diag.UartFrameCount"
UART_ERRORS = f"ns={NS};s=Diag.UartErrorCount"


class Sender:
    def __init__(self, port: str, rate: float):
        self.ser = serial.Serial(port, 115200, timeout=0)
        self.period = 1.0 / rate
        self.sent = {}                       # seq -> perf_counter at write
        self.last = 0
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._t.start()

    def stop(self):
        self._stop.set()
        self._t.join()
        self.ser.close()

    def _run(self):
        seq = 0
        nxt = time.perf_counter()
        while not self._stop.is_set():
            seq += 1
            line = f"$DATA:{seq},{seq % 1000 + 0.5},{2 * seq + 1}\n"
            self.sent[seq] = time.perf_counter()
            self.ser.write(line.encode())
            self.last = seq
            nxt += self.period
            delay = nxt - time.perf_counter()
            if delay > 0:
                time.sleep(delay)


class Watcher:
    """One client: a subscription on ch1, and periodic snapshot reads."""

    def __init__(self, idx: int, sender: Sender):
        self.idx = idx
        self.sender = sender
        self.seen = {}                       # seq -> arrival perf_counter
        self.snapshots = 0
        self.integrity_bad = []
        self.final = None
        self.error = None
        self.losses = []                     # perf_counter of each detected loss

    async def on_lost(self, exc):
        self.losses.append(time.perf_counter())
        print(f"client {self.idx} lost connection at {time.strftime('%H:%M:%S')}: {exc!r}")

    def datachange_notification(self, node, val, data):
        try:
            seq = int(round(float(val)))
        except (TypeError, ValueError):
            return
        if seq > 0 and seq not in self.seen:
            self.seen[seq] = time.perf_counter()

    async def run(self, args, stop: asyncio.Event):
        # asyncua 2.x probes ServerState every watchdog_intervall with a
        # timeout of the same length and drops the connection on a miss. The
        # board's loop stalls about 0.8 s per RSA operation while another
        # client handshakes or renews, so the 1 s default trips on load alone.
        client = Client(url=args.url, timeout=15, watchdog_intervall=args.watchdog,
                        auto_reconnect=args.reconnect)
        client.connection_lost_callback = self.on_lost
        if args.secure:
            client.application_uri = APP_URI
            await client.set_security_string(f"{POLICY},{CERT},{KEY}")
        if args.lifetime:
            client.secure_channel_timeout = args.lifetime
        try:
            async with client:
                print(f"client {self.idx} connected at {time.strftime('%H:%M:%S')}")
                nodes = [client.get_node(n) for n in CH]
                sub = await client.create_subscription(100, self)
                await sub.subscribe_data_change(nodes[0], sampling_interval=50)
                while not stop.is_set():
                    try:
                        vals = await client.read_values(nodes)
                    except ua.UaStatusCodeError:
                        # BadWaitingForInitialData until the first frame lands.
                        await asyncio.sleep(1.0)
                        continue
                    except (ConnectionError, asyncio.TimeoutError):
                        if not args.reconnect:
                            raise
                        await asyncio.sleep(1.0)     # supervisor is reconnecting
                        continue
                    self.snapshots += 1
                    # None until the first frame lands (Bad_WaitingForInitialData).
                    if vals[0] is not None and vals[2] is not None:
                        a, c = float(vals[0]), float(vals[2])
                        if a > 0 and c != 2 * a + 1:
                            self.integrity_bad.append((a, vals[1], c))
                    await asyncio.sleep(1.0)
                await asyncio.sleep(1.5)     # let the last frame land
                try:
                    last = await nodes[0].read_value()
                except ua.UaStatusCodeError:
                    last = None
                self.final = None if last is None else int(round(float(last)))
        except Exception as exc:             # noqa: BLE001 - reporting tool
            self.error = (f"{type(exc).__name__}: {exc} at "
                          f"{time.strftime('%H:%M:%S')}, "
                          f"{len(self.seen)} notifications, {self.snapshots} reads")


async def read_counter(url: str, node: str) -> int:
    c = Client(url=url, timeout=10)
    async with c:
        return int(await c.get_node(node).read_value())


async def main(args) -> int:
    frames0 = await read_counter(args.url, UART_FRAMES)
    errors0 = await read_counter(args.url, UART_ERRORS)

    sender = Sender(args.port, args.rate)
    watchers = [Watcher(i, sender) for i in range(args.clients)]
    stop = asyncio.Event()
    tasks = []
    for w in watchers:
        tasks.append(asyncio.create_task(w.run(args, stop)))
        await asyncio.sleep(2.0)

    sender.start()
    started = time.perf_counter()
    await asyncio.sleep(args.seconds)
    sender.stop()
    stop.set()
    await asyncio.gather(*tasks)
    elapsed = time.perf_counter() - started

    frames1 = await read_counter(args.url, UART_FRAMES)
    errors1 = await read_counter(args.url, UART_ERRORS)
    sent = sender.last
    parsed = frames1 - frames0

    print(f"{sent} frames sent over {elapsed:.1f} s at {args.rate} Hz, "
          f"{'Basic256Sha256' if args.secure else 'None'}, "
          f"{args.clients} client(s), lifetime {args.lifetime or 'default'}")
    print(f"board parsed {parsed} frames, {errors1 - errors0} UART errors")

    ok = parsed == sent and errors1 == errors0
    for w in watchers:
        if w.error:
            print(f"client {w.idx}: ERROR {w.error}")
            ok = False
            continue
        lat = [(t - sender.sent[s]) * 1000 for s, t in w.seen.items()
               if s in sender.sent]
        lat.sort()
        p50 = statistics.median(lat) if lat else float("nan")
        p95 = lat[int(len(lat) * 0.95) - 1] if len(lat) >= 20 else float("nan")
        final_ok = w.final == sent
        print(f"client {w.idx}: final={w.final} ({'ok' if final_ok else 'MISMATCH'})  "
              f"notified {len(w.seen)}/{sent} ({100 * len(w.seen) / max(sent, 1):.0f} %)  "
              f"latency p50 {p50:.0f} ms p95 {p95:.0f} ms  "
              f"snapshots {w.snapshots}, integrity bad {len(w.integrity_bad)}")
        if w.integrity_bad:
            print(f"   first bad snapshot: {w.integrity_bad[0]}")
        ok = ok and final_ok and not w.integrity_bad
        if args.reconnect:
            # Restore is proven by notifications arriving after the last loss,
            # not by the final Read (a fresh session reads fine without one).
            arrivals = sorted(w.seen.values())
            gaps = [b - a for a, b in zip(arrivals, arrivals[1:])]
            after = sum(1 for t in arrivals if w.losses and t > w.losses[-1])
            print(f"   losses {len(w.losses)}, longest notification gap "
                  f"{max(gaps, default=0):.1f} s, notified after last loss {after}")
            ok = ok and bool(w.losses) and after > 0
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rate", type=float, default=10.0)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--clients", type=int, default=1)
    ap.add_argument("--secure", action="store_true")
    ap.add_argument("--lifetime", type=int, default=0)
    ap.add_argument("--reconnect", action="store_true",
                    help="auto-reconnect; expects a link outage during the run")
    ap.add_argument("--watchdog", type=float, default=5.0,
                    help="asyncua keepalive probe interval and timeout, s")
    ap.add_argument("--url", default=DEFAULT_URL)
    raise SystemExit(asyncio.run(main(ap.parse_args())))
