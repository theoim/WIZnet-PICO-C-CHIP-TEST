#!/usr/bin/env python3
"""Long-running soak of the OPC UA node: data path, sessions, memory, faults.

    python tools/opcua_soak_test.py --uart COM3 --console COM12 --duration 12h
    python tools/opcua_soak_test.py --uart COM3 --console COM12 --duration 10m \\
           --churn 60 --kill 180 --fault-every 240 --fault-offset 100 \\
           --summary-every 120 --heap-window 180          # rehearsal

Five jobs run side by side for the whole duration:

  uart      $DATA:<seq>,<seq mod 1000 + 0.5>,<2*seq + 1> at --rate Hz on the
            USB-TTL port, into the board's UART0.
  clients   --clients secure sessions, each subscribed to ch1 and reading all
            three channels once a second; auto-reconnect on.
  churn     every --churn s a secure client connects, reads, and closes
            properly; every --kill s a client process is killed instead, which
            leaves its session behind without CloseSession.
  sampler   every --sample s the board's Diag nodes go to a CSV row.
  console   the board's USB console (COM12) is logged line by line; scheduled
            link outages are injected through it (LINKDOWN, PHY power-down).

Judged separately, because they answer different questions:

  validity      the board runs the expected build, no CSV row is missing, the
                script itself raised nothing. A FAIL here means the run proves
                nothing either way.
  board         no reboot (RebootCount unchanged, Uptime never goes back).
  data          board parsed every frame sent, no UART errors; every snapshot
                is one frame (ch2, ch3 follow from ch1); no client sees a seq
                twice or going backwards; the final value is the last seq.
                Notification count is not compared with frames: a subscription
                samples, it does not queue.
  clients       any loss or notification stall outside an injected fault is a
                FAIL. Injected faults are judged by recovery time.
  normal churn  every connect outside fault and reclaim windows must succeed.
  intentional   killed sessions must be reclaimed (SessionCount back to the
                baseline) in time; each link outage must be recovered from
                (notifications resume) within --recover-max.
  memory        FreeHeap compared only between samples taken in the same
                session/channel/socket state, first window against last.
"""

import argparse
import asyncio
import csv
import datetime as dt
import logging
import os
import statistics
import subprocess
import sys
import threading
import time
import traceback

import serial
from asyncua import Client, ua

from opcua_concurrent_test import APP_URI, CERT, DEFAULT_URL, KEY, POLICY

NS = 2
CH = [f"ns={NS};s=Channels/Channel_{i}" for i in (1, 2, 3)]
DIAG = ["BuildId", "Uptime_s", "RebootCount", "RebootReason", "FreeHeap",
        "MinFreeHeap", "SessionCount", "ChannelCount", "SocketsBusy",
        "HandshakeRefused", "CloseCut", "ChipDropped", "UartFrameCount",
        "UartErrorCount", "LinkStatus"]
DIAG_IDS = [f"ns={NS};s=Diag.{n}" for n in DIAG]
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def now_utc() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def parse_duration(text: str) -> float:
    unit = {"s": 1, "m": 60, "h": 3600}.get(text[-1])
    return float(text[:-1]) * unit if unit else float(text)


def expected_build() -> str:
    try:
        return subprocess.check_output(
            ["git", "describe", "--always", "--dirty", "--abbrev=7"],
            cwd=REPO, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "no-git"


def secure(client: Client):
    client.application_uri = APP_URI
    return client.set_security_string(f"{POLICY},{CERT},{KEY}")


class Run:
    """Shared state: the clock, the event log, and every judged counter."""

    def __init__(self, args, outdir):
        self.args = args
        self.t0 = time.perf_counter()
        self.outdir = outdir
        self.events = open(os.path.join(outdir, "events.log"), "a", encoding="utf-8")
        self.script_errors = []
        self.faults = []          # dicts: start, end (PHY up), recovered[]
        self.kills = []           # dicts: t, reclaim_s or None
        # (start, end) spans where trouble is expected. A link outage excuses
        # client losses and churn failures; a killed session's reclaim span
        # excuses only churn - it must not disturb the long-lived clients.
        self.fault_windows = []
        self.reclaim_windows = []
        self.churn = {"ok": 0, "fail": 0, "refused": 0, "fail_msgs": [],
                      "win_ok": 0, "win_fail": 0}
        self.stop = asyncio.Event()

    def t(self) -> float:
        return time.perf_counter() - self.t0

    def log(self, msg: str):
        line = f"{now_utc()} +{self.t():8.1f}s {msg}"
        print(line, flush=True)
        self.events.write(line + "\n")
        self.events.flush()

    def error(self, where: str, exc: BaseException):
        self.script_errors.append(f"{where}: {type(exc).__name__}: {exc}")
        self.log(f"SCRIPT ERROR in {where}: {type(exc).__name__}: {exc}")
        with open(os.path.join(self.outdir, "script_errors.log"), "a",
                  encoding="utf-8") as f:
            f.write(f"--- {now_utc()} {where}\n{traceback.format_exc()}\n")

    def in_fault(self, t: float, pad: float = 0.0) -> bool:
        return any(a - pad <= t <= b + pad for a, b in self.fault_windows)

    def overlaps_fault(self, start: float, end: float) -> bool:
        return any(start <= b and a <= end for a, b in self.fault_windows)

    def in_window(self, t: float, pad: float = 0.0) -> bool:
        return self.in_fault(t, pad) or any(
            a - pad <= t <= b + pad for a, b in self.reclaim_windows)


async def guard(run: Run, name: str, coro):
    try:
        await coro
    except asyncio.CancelledError:
        raise
    except Exception as exc:                             # noqa: BLE001
        run.error(name, exc)


# ---------------------------------------------------------------- uart job
class Sender:
    def __init__(self, run: Run, port: str, rate: float):
        self.run = run
        self.ser = serial.Serial(port, 115200, timeout=0)
        self.period = 1.0 / rate
        self.sent_at = {}
        self.last = 0
        self.failed = None
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._loop, daemon=True)

    def start(self):
        self._t.start()

    def stop(self):
        self._stop.set()
        self._t.join()
        self.ser.close()

    def _loop(self):
        seq, nxt = 0, time.perf_counter()
        try:
            while not self._stop.is_set():
                seq += 1
                self.sent_at[seq] = time.perf_counter()
                self.ser.write(f"$DATA:{seq},{seq % 1000 + 0.5},{2 * seq + 1}\n".encode())
                self.last = seq
                nxt += self.period
                delay = nxt - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
        except Exception as exc:                         # noqa: BLE001
            self.failed = f"{type(exc).__name__}: {exc}"


# ------------------------------------------------------------- console job
class Console:
    PATTERNS = {
        "reboot": "=== WIZnet OPC UA Node",
        "link_down": "PHY link down",
        "link_up": "PHY link up",
        "phy_up": "PHY powered up",
        "chip_drop": "dropped by chip",
        "close_cut": "close cut",
        "refused": "handshake refused",
        "watchdog": "[wdt]",
    }

    def __init__(self, run: Run, port: str):
        self.run = run
        self.port = port
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.lost = 0                   # times the port vanished (USB reset)
        self.f = open(os.path.join(run.outdir, "console.log"), "a", encoding="utf-8")
        self.counts = {k: 0 for k in self.PATTERNS}
        self.last_line_t = run.t()
        self.phy_up_at = []
        self.failed = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._loop, daemon=True)

    def start(self):
        self._t.start()

    def stop(self):
        self._stop.set()
        self._t.join()
        self.ser.close()
        self.f.close()

    def send(self, cmd: str) -> bool:
        try:
            with self._lock:
                self.ser.write((cmd + "\r\n").encode())
        except serial.SerialException as exc:
            self.run.log(f"console <- {cmd} NOT SENT: {exc}")
            return False
        self.run.log(f"console <- {cmd}")
        return True

    def _reopen(self):
        """A board reset re-enumerates USB and the old handle is dead."""
        self.lost += 1
        self.run.log("console port lost (board USB reset?); reopening")
        with self._lock:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
        while not self._stop.is_set():
            time.sleep(1.0)
            try:
                ser = serial.Serial(self.port, 115200, timeout=0.2)
            except serial.SerialException:
                continue
            with self._lock:
                self.ser = ser
            self.run.log("console port reopened")
            return

    def _loop(self):
        buf = b""
        try:
            while not self._stop.is_set():
                try:
                    with self._lock:
                        chunk = self.ser.read(4096)
                except serial.SerialException:
                    self._reopen()
                    continue
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    if not line.strip():
                        continue
                    t = self.run.t()
                    self.last_line_t = t
                    self.f.write(f"{now_utc()} +{t:8.1f}s {line}\n")
                    self.f.flush()
                    for key, pat in self.PATTERNS.items():
                        if pat in line:
                            self.counts[key] += 1
                            if key == "phy_up":
                                self.phy_up_at.append(t)
                            if key in ("reboot", "watchdog", "close_cut"):
                                self.run.log(f"console event {key}: {line}")
        except Exception as exc:                         # noqa: BLE001
            self.failed = f"{type(exc).__name__}: {exc}"


# ------------------------------------------------------------- client job
class LongClient:
    def __init__(self, run: Run, idx: int, sender: Sender):
        self.run, self.idx, self.sender = run, idx, sender
        self.client = None
        self.connected = False
        self.last_seq = 0
        self.last_snap = 0
        self.allow_repeat = True        # first value after (re)connect may repeat
        self.notifs = 0
        self.arrivals = []              # perf-relative arrival times (thinned)
        self.last_arrival = None
        self.losses = []                # (t, intentional, exc)
        self.stalls = []                # (start, length) outside windows
        self.seq_dup = 0
        # A repeat inside --recover-max of a loss: after a reconnect the server
        # may send an unacknowledged notification message again. Counted and
        # logged, not judged; a repeat anywhere else is.
        self.seq_dup_reconnect = 0
        self.seq_back = 0
        self.integrity_bad = []
        self.snapshots = 0
        self.final = None

    # asyncua subscription handler
    def datachange_notification(self, node, val, data):
        try:
            seq = int(round(float(val)))
        except (TypeError, ValueError):
            return
        # A value above the last seq sent is the previous run's, still in
        # the board until this run's first frame lands.
        if seq <= 0 or seq > self.sender.last:
            return
        t = self.run.t()
        if self.last_arrival is not None:
            gap = t - self.last_arrival
            if gap > self.run.args.stall and not self.run.overlaps_fault(
                    self.last_arrival, t):
                self.stalls.append((self.last_arrival, gap))
                self.run.log(f"client {self.idx} notification stall {gap:.1f} s "
                             f"(outside any injected fault)")
        self.last_arrival = t
        self.notifs += 1
        if seq == self.last_seq:
            after_loss = bool(self.losses) and t - self.losses[-1][0] <= self.run.args.recover_max
            if self.allow_repeat:
                pass
            elif after_loss:
                self.seq_dup_reconnect += 1
                self.run.log(f"client {self.idx} seq {seq} repeated "
                             f"{t - self.losses[-1][0]:.1f} s after a loss")
            else:
                self.seq_dup += 1
                self.run.log(f"client {self.idx} seq {seq} DUPLICATE")
        elif seq < self.last_seq:
            self.seq_back += 1
            self.run.log(f"client {self.idx} seq went back {self.last_seq} -> {seq}")
        self.allow_repeat = False
        self.last_seq = max(self.last_seq, seq)
        for f in self.run.faults:
            if f.get("end") and t > f["end"] and self.idx not in f["recovered"]:
                f["recovered"][self.idx] = t - f["end"]

    async def on_lost(self, exc):
        t = self.run.t()
        intentional = self.run.in_fault(t, pad=2.0)
        self.losses.append((t, intentional, repr(exc)))
        self.allow_repeat = True
        self.run.log(f"client {self.idx} lost connection "
                     f"({'injected fault' if intentional else 'UNEXPECTED'}): {exc!r}")

    async def run_forever(self):
        args = self.run.args
        c = Client(url=args.url, timeout=15, watchdog_intervall=args.watchdog,
                   auto_reconnect=True)
        await secure(c)
        c.connection_lost_callback = self.on_lost
        self.client = c
        async with c:
            self.connected = True
            self.run.log(f"client {self.idx} connected")
            nodes = [c.get_node(n) for n in CH]
            sub = await c.create_subscription(100, self)
            await sub.subscribe_data_change(nodes[0], sampling_interval=50)
            while not self.run.stop.is_set():
                try:
                    vals = await c.read_values(nodes)
                except ua.UaStatusCodeError:
                    await asyncio.sleep(1.0)     # BadWaitingForInitialData
                    continue
                except (ConnectionError, asyncio.TimeoutError):
                    await asyncio.sleep(1.0)     # supervisor is reconnecting
                    continue
                self.snapshots += 1
                if vals[0] is not None and vals[2] is not None:
                    a, b, cc = float(vals[0]), float(vals[1]), float(vals[2])
                    if a > self.sender.last:
                        await asyncio.sleep(1.0)     # previous run's value
                        continue
                    if a > 0 and (cc != 2 * a + 1 or b != a % 1000 + 0.5):
                        self.integrity_bad.append((a, b, cc))
                        self.run.log(f"client {self.idx} INTEGRITY {a}, {b}, {cc}")
                    if a < self.last_snap:
                        self.seq_back += 1
                    self.last_snap = max(self.last_snap, a)
                await asyncio.sleep(1.0)
            await asyncio.sleep(2.0)             # let the last frame land
            try:
                self.final = int(round(float(await nodes[0].read_value())))
            except Exception as exc:             # noqa: BLE001
                self.run.log(f"client {self.idx} final read failed: {exc!r}")
        self.connected = False

    async def read_diag(self):
        if not self.client or not self.connected:
            return None
        try:
            return await asyncio.wait_for(
                self.client.read_values([self.client.get_node(n) for n in DIAG_IDS]),
                timeout=10)
        except Exception:                         # noqa: BLE001 - try the other one
            return None


# --------------------------------------------------------------- churn job
async def churn_once(run: Run) -> tuple:
    c = Client(url=run.args.url, timeout=15, watchdog_intervall=3600.0)
    await secure(c)
    started = time.perf_counter()
    try:
        await c.connect()
        try:
            await c.read_values([c.get_node(n) for n in CH])
        finally:
            await c.disconnect()
        return "ok", time.perf_counter() - started
    except Exception as exc:                      # noqa: BLE001
        msg = f"{type(exc).__name__}: {exc}"
        return ("refused" if "TooBusy" in msg else "fail"), msg


async def kill_once(run: Run, watch) -> None:
    """Start a client process, let it open a session, kill it."""
    t = run.t()
    proc = await asyncio.create_subprocess_exec(
        sys.executable, os.path.abspath(__file__), "--kill-child", run.args.url,
        cwd=os.path.dirname(os.path.abspath(__file__)),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
    try:
        line = await asyncio.wait_for(proc.stdout.readline(), timeout=40)
    except asyncio.TimeoutError:
        line = b""
    proc.kill()
    await proc.wait()
    rec = {"t": t, "reclaim_s": None, "ready": line.startswith(b"READY")}
    run.kills.append(rec)
    if not rec["ready"]:
        run.log("kill: child never opened its session (counted as churn fail)")
        run.churn["fail"] += 1
        run.churn["fail_msgs"].append(f"+{t:.0f}s kill child not ready")
        return
    killed = run.t()
    run.reclaim_windows.append((killed, killed + run.args.reclaim_max))
    run.log("kill: client process killed with its session open")
    baseline = run.args.clients
    while run.t() - killed < run.args.reclaim_max and not run.stop.is_set():
        await asyncio.sleep(5.0)
        vals = await watch()
        if vals is None:
            continue
        if int(vals[DIAG.index("SessionCount")]) <= baseline:
            rec["reclaim_s"] = run.t() - killed
            run.log(f"kill: orphan session reclaimed after {rec['reclaim_s']:.0f} s")
            return
    run.log("kill: orphan session NOT reclaimed in time")


def kill_child(url: str) -> None:
    async def go():
        c = Client(url=url, timeout=15, watchdog_intervall=3600.0)
        await secure(c)
        c.session_timeout = 30000
        await c.connect()
        print("READY", flush=True)
        await asyncio.sleep(3600)
    asyncio.run(go())


async def churn_job(run: Run, watch):
    a = run.args
    tick = 0
    while True:
        tick += 1
        if await sleep_until(run, tick * a.churn):
            return
        t = run.t()
        if a.kill and int(t + 1) // a.kill > int(t - a.churn + 1) // a.kill:
            asyncio.create_task(guard(run, "kill", kill_once(run, watch)))
            continue
        result, detail = await churn_once(run)
        in_win = run.in_window(t, pad=5.0) or run.in_window(run.t(), pad=5.0)
        if in_win:
            run.churn["win_ok" if result == "ok" else "win_fail"] += 1
            if result != "ok":
                run.log(f"churn during injected window: {result} {detail}")
        else:
            run.churn[result] += 1
            if result != "ok":
                run.churn["fail_msgs"].append(f"+{t:.0f}s {result}: {detail}")
                run.log(f"churn {result}: {detail}")


async def sleep_until(run: Run, t_rel: float) -> bool:
    """Sleep until t_rel after start; True if the run is stopping."""
    delay = t_rel - run.t()
    if delay > 0:
        try:
            await asyncio.wait_for(run.stop.wait(), timeout=delay)
        except asyncio.TimeoutError:
            pass
    return run.stop.is_set()


# --------------------------------------------------------------- fault job
async def fault_job(run: Run, console: Console):
    a = run.args
    k = 0
    while True:
        start = a.fault_offset + k * a.fault_every
        k += 1
        if start + a.fault_len + a.recover_max > a.duration_s - 30:
            return
        if await sleep_until(run, start):
            return
        t = run.t()
        f = {"start": t, "end": None, "recovered": {}}
        run.faults.append(f)
        run.fault_windows.append((t, t + a.fault_len + a.recover_max))
        n_up = len(console.phy_up_at)
        if not console.send(f"LINKDOWN {a.fault_len}"):
            f["end"] = t
            f["not_injected"] = True
            continue
        while len(console.phy_up_at) == n_up and run.t() - t < a.fault_len + 15:
            await asyncio.sleep(0.5)
        f["end"] = console.phy_up_at[-1] if len(console.phy_up_at) > n_up else t + a.fault_len
        run.log(f"fault: PHY back up after {f['end'] - t:.1f} s")


# ------------------------------------------------------------- sampler job
CSV_COLS = (["utc", "t_s", "src"] + DIAG +
            ["uart_sent", "state", "in_window", "churn_ok", "churn_fail",
             "churn_refused", "kills", "faults", "losses_unexpected",
             "losses_injected", "notifs", "integrity_bad", "seq_dup", "seq_back",
             "script_errors"])


class Sampler:
    def __init__(self, run: Run, clients, sender: Sender, expect: str):
        self.run, self.clients, self.sender, self.expect = run, clients, sender, expect
        path = os.path.join(run.outdir, "samples.csv")
        self.f = open(path, "a", newline="", encoding="utf-8")
        self.w = csv.writer(self.f)
        self.w.writerow(CSV_COLS)
        self.rows = []

    async def watch(self):
        for c in self.clients:
            vals = await c.read_diag()
            if vals is not None:
                return vals
        return None

    async def job(self):
        a = self.run.args
        k = 0
        while True:
            k += 1
            if await sleep_until(self.run, k * a.sample - a.sample / 2):
                return
            self.write(await self.sample_once())

    async def sample_once(self):
        src, vals = "none", None
        for c in self.clients:
            vals = await c.read_diag()
            if vals is not None:
                src = f"c{c.idx}"
                break
        d = dict(zip(DIAG, vals)) if vals else {}
        state = (f"{d['SessionCount']}/{d['ChannelCount']}/{d['SocketsBusy']}"
                 if d else "")
        return src, d, state

    def write(self, sample):
        src, d, state = sample
        r, cl = self.run, self.clients
        row = {
            "utc": now_utc(), "t_s": round(r.t(), 1), "src": src,
            **{k: d.get(k, "") for k in DIAG},
            "uart_sent": self.sender.last, "state": state,
            "in_window": int(r.in_window(r.t())),
            "churn_ok": r.churn["ok"], "churn_fail": r.churn["fail"],
            "churn_refused": r.churn["refused"], "kills": len(r.kills),
            "faults": len(r.faults),
            "losses_unexpected": sum(1 for c in cl for l in c.losses if not l[1]),
            "losses_injected": sum(1 for c in cl for l in c.losses if l[1]),
            "notifs": "/".join(str(c.notifs) for c in cl),
            "integrity_bad": sum(len(c.integrity_bad) for c in cl),
            "seq_dup": sum(c.seq_dup for c in cl),
            "seq_back": sum(c.seq_back for c in cl),
            "script_errors": len(r.script_errors),
        }
        self.w.writerow([row[k] for k in CSV_COLS])
        self.f.flush()
        self.rows.append(row)
        if d and str(d.get("BuildId", "")).split(" ")[0] != self.expect:
            r.log(f"BUILD MISMATCH mid-run: board {d.get('BuildId')!r}")


# ----------------------------------------------------------------- verdict
def verdict(run: Run, sampler: Sampler, sender: Sender, console: Console,
            clients, base, final, expect: str, done: bool) -> tuple:
    a, out, ok = run.args, [], True

    def check(cond, label, detail):
        nonlocal ok
        ok = ok and cond
        out.append(f"  [{'PASS' if cond else 'FAIL'}] {label}: {detail}")

    rows = [r for r in sampler.rows if r["src"] != "none"]
    out.append("validity")
    builds = {str(r["BuildId"]).split(" ")[0] for r in rows}
    check(builds <= {expect} and base["BuildId"].split(" ")[0] == expect,
          "build", f"expected {expect}, board {sorted(builds | {base['BuildId']})}")
    ts = [r["t_s"] for r in sampler.rows]
    gaps = [b - x for x, b in zip(ts, ts[1:])]
    want = int(run.t() // a.sample) - 1
    check((max(gaps, default=0) <= 2 * a.sample) and len(ts) >= want,
          "csv rows", f"{len(ts)} rows (want >= {want}), longest gap "
          f"{max(gaps, default=0):.0f} s, {len(sampler.rows) - len(rows)} without board data")
    errs = run.script_errors + ([f"uart: {sender.failed}"] if sender.failed else []) + \
        ([f"console: {console.failed}"] if console.failed else [])
    check(not errs, "script errors", f"{len(errs)}" + (f" first: {errs[0]}" if errs else ""))

    out.append("board")
    rc = [int(r["RebootCount"]) for r in rows]
    up = [int(r["Uptime_s"]) for r in rows]
    check(all(x == base["RebootCount"] for x in rc) and console.counts["reboot"] == 0,
          "reboots", f"RebootCount {base['RebootCount']} -> {rc[-1] if rc else '?'}, "
          f"boot banners seen {console.counts['reboot']}")
    check(all(y >= x for x, y in zip(up, up[1:])), "uptime monotonic",
          f"{up[0] if up else '?'} -> {up[-1] if up else '?'} s")

    out.append("data")
    if final:
        parsed = final["UartFrameCount"] - base["UartFrameCount"]
        uerr = final["UartErrorCount"] - base["UartErrorCount"]
        check(parsed == sender.last and uerr == 0, "uart frames",
              f"sent {sender.last}, board parsed {parsed}, UART errors {uerr}")
    elif done:
        check(False, "uart frames", "no final board read")
    ib = sum(len(c.integrity_bad) for c in clients)
    check(ib == 0, "snapshot integrity",
          f"{sum(c.snapshots for c in clients)} snapshots, {ib} bad")
    dup, back = sum(c.seq_dup for c in clients), sum(c.seq_back for c in clients)
    check(dup == 0 and back == 0, "seq order", f"duplicates {dup}, went back {back}")
    out.append(f"  [info] repeats right after a reconnect: "
               f"{sum(c.seq_dup_reconnect for c in clients)}")
    if done:
        finals = [c.final for c in clients]
        check(all(f == sender.last for f in finals), "final value",
              f"last seq {sender.last}, clients read {finals}")

    out.append("clients")
    unexpected = [(c.idx, l) for c in clients for l in c.losses if not l[1]]
    stalls = [(c.idx, s) for c in clients for s in c.stalls]
    check(not unexpected, "unexpected losses",
          f"{len(unexpected)}" + "".join(f"; c{i} at +{l[0]:.0f}s {l[2]}" for i, l in unexpected[:3]))
    check(not stalls, "notification stalls > %gs" % a.stall,
          f"{len(stalls)}" + "".join(f"; c{i} at +{s[0]:.0f}s for {s[1]:.0f}s" for i, s in stalls[:3]))

    out.append("normal churn")
    ch = run.churn
    n = ch["ok"] + ch["fail"] + ch["refused"]
    check(ch["fail"] == 0 and ch["refused"] == 0, "connect outside windows",
          f"{ch['ok']}/{n} ok, {ch['refused']} TooBusy, {ch['fail']} failed"
          + (f"; first: {ch['fail_msgs'][0]}" if ch["fail_msgs"] else ""))

    out.append("intentional")
    kr = [k["reclaim_s"] for k in run.kills if k["ready"]]
    settled = [k for k in run.kills if k["ready"] and
               (done or run.t() - k["t"] > a.reclaim_max + 45)]
    check(all(k["reclaim_s"] is not None for k in settled), "killed sessions reclaimed",
          f"{len(run.kills)} kills, reclaim s {['%.0f' % x if x is not None else 'NO' for x in kr]} "
          f"(limit {a.reclaim_max:.0f} s)")
    rec = []
    for f in run.faults:
        if f.get("not_injected"):
            rec.append(["not injected"])
            continue
        if not done and run.t() - f["start"] < a.fault_len + a.recover_max + 5:
            continue
        times = [f["recovered"].get(c.idx) for c in clients]
        rec.append(times)
    check(all(isinstance(x, float) and x <= a.recover_max for t in rec for x in t),
          "link outage recovery",
          f"{len(run.faults)} outages of {a.fault_len} s, per-client recovery s "
          f"{[['%.1f' % x if isinstance(x, float) else str(x or 'NO') for x in t] for t in rec]} "
          f"(limit {a.recover_max:.0f} s)")
    out.append(f"  [info] churn inside injected windows: {ch['win_ok']} ok, {ch['win_fail']} not ok")

    out.append("memory")
    key = f"{a.clients}/{a.clients}/{a.clients}"
    steady = [r for r in rows if r["state"] == key and not r["in_window"]]
    first = [int(r["FreeHeap"]) for r in steady if r["t_s"] <= a.heap_window]
    last = [int(r["FreeHeap"]) for r in steady if r["t_s"] >= run.t() - a.heap_window]
    mins = [int(r["MinFreeHeap"]) for r in rows]
    if done and len(first) >= 2 and len(last) >= 2:
        drop = statistics.median(first) - statistics.median(last)
        check(drop < a.heap_drop_max, f"FreeHeap in state {key}",
              f"first {statistics.median(first):.0f}, last {statistics.median(last):.0f}, "
              f"drop {drop:.0f} B (limit {a.heap_drop_max})")
    elif done:
        check(False, f"FreeHeap in state {key}",
              f"too few steady samples (first {len(first)}, last {len(last)})")
    else:
        out.append(f"  [info] FreeHeap in state {key}: {len(steady)} samples, "
                   f"latest {steady[-1]['FreeHeap'] if steady else '?'}")
    out.append(f"  [info] MinFreeHeap {min(mins) if mins else '?'} B; "
               f"states seen {sorted({r['state'] for r in rows})}")
    out.append(f"  [info] console: " + ", ".join(f"{k} {v}" for k, v in console.counts.items())
               + f", port lost {console.lost}")
    return ok, out


def write_summary(run, lines, ok, label):
    text = (f"=== {label} {now_utc()} (+{run.t() / 3600:.2f} h) "
            f"{'PASS' if ok else 'FAIL'} ===\n" + "\n".join(lines) + "\n")
    print(text, flush=True)
    with open(os.path.join(run.outdir, "summary.txt"), "a", encoding="utf-8") as f:
        f.write(text + "\n")


# -------------------------------------------------------------------- main
async def read_board(url: str) -> dict:
    c = Client(url=url, timeout=15)
    async with c:
        vals = await c.read_values([c.get_node(n) for n in DIAG_IDS])
    return dict(zip(DIAG, vals))


async def main(args) -> int:
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M")
    outdir = os.path.join(args.out, f"soak_{stamp}")
    os.makedirs(outdir, exist_ok=True)
    logging.basicConfig(filename=os.path.join(outdir, "asyncua.log"),
                        level=logging.WARNING,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    run = Run(args, outdir)
    expect = args.expect_build or expected_build()
    run.log(f"soak start: {args.duration} ({run.args.duration_s:.0f} s), out {outdir}")
    run.log(f"settings: {vars(args)}")

    try:
        base = await read_board(args.url)
    except Exception as exc:                      # noqa: BLE001
        run.log(f"ABORT: cannot read the board's Diag nodes ({exc!r}) - "
                f"old firmware without BuildId, or board not reachable")
        return 2
    board_build = str(base["BuildId"]).split(" ")[0]
    run.log(f"board build {base['BuildId']!r}, expected {expect}")
    if board_build != expect:
        run.log("ABORT: build mismatch - flash the image built from this tree")
        return 2
    # The probe read above holds one session and one socket of its own.
    if int(base["SessionCount"]) > 1 or int(base["SocketsBusy"]) > 1:
        run.log(f"ABORT: board not idle (sessions {base['SessionCount']}, "
                f"sockets busy {base['SocketsBusy']}) - wait for old sessions to expire")
        return 2

    try:
        console = Console(run, args.console)
        sender = Sender(run, args.uart, args.rate)
    except serial.SerialException as exc:
        run.log(f"ABORT: {exc} - close any terminal holding the port")
        return 2
    console.start()

    clients = [LongClient(run, i, sender) for i in range(args.clients)]
    sampler = Sampler(run, clients, sender, expect)
    tasks = []
    for c in clients:
        tasks.append(asyncio.create_task(guard(run, f"client {c.idx}", c.run_forever())))
        await asyncio.sleep(3.0)
    sender.start()
    run.t0 = time.perf_counter()        # schedules count from the first frame
    run.log("uart sending; all jobs running")
    jobs = [asyncio.create_task(guard(run, "sampler", sampler.job())),
            asyncio.create_task(guard(run, "churn", churn_job(run, sampler.watch)))]
    if args.fault_every:
        jobs.append(asyncio.create_task(guard(run, "fault", fault_job(run, console))))

    nxt = args.summary_every
    while run.t() < args.duration_s:
        await asyncio.sleep(min(5.0, max(0.1, args.duration_s - run.t())))
        if run.t() >= nxt:
            ok, lines = verdict(run, sampler, sender, console, clients, base, None,
                                expect, done=False)
            write_summary(run, lines, ok, "interim")
            nxt += args.summary_every

    run.log("duration reached; draining")
    sender.stop()
    run.stop.set()
    await asyncio.gather(*jobs)
    await asyncio.gather(*tasks)
    final = None
    try:
        final = await read_board(args.url)
    except Exception as exc:                      # noqa: BLE001
        run.error("final read", exc)
    sampler.write(("final", final or {}, ""))
    console.stop()
    ok, lines = verdict(run, sampler, sender, console, clients, base, final,
                        expect, done=True)
    write_summary(run, lines, ok, "FINAL")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--kill-child":
        kill_child(sys.argv[2])
        raise SystemExit(0)
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--uart", required=True, help="USB-TTL port wired to UART0")
    ap.add_argument("--console", required=True, help="board USB console port")
    ap.add_argument("--duration", default="12h", help="e.g. 12h, 10m, 600")
    ap.add_argument("--rate", type=float, default=10.0)
    ap.add_argument("--clients", type=int, default=2)
    ap.add_argument("--churn", type=float, default=120.0, help="graceful connect period, s")
    ap.add_argument("--kill", type=float, default=600.0, help="killed-client period, s (0 off)")
    ap.add_argument("--reclaim-max", type=float, default=90.0,
                    help="killed session must be gone within, s (child asks 30 s timeout)")
    ap.add_argument("--fault-every", type=float, default=7200.0, help="link outage period, s (0 off)")
    ap.add_argument("--fault-offset", type=float, default=1800.0)
    ap.add_argument("--fault-len", type=int, default=20)
    ap.add_argument("--recover-max", type=float, default=60.0)
    ap.add_argument("--stall", type=float, default=15.0, help="notification gap judged a stall, s")
    ap.add_argument("--sample", type=float, default=60.0)
    ap.add_argument("--summary-every", type=float, default=3600.0)
    ap.add_argument("--heap-window", type=float, default=3600.0,
                    help="first/last window compared for FreeHeap, s")
    ap.add_argument("--heap-drop-max", type=int, default=4096)
    ap.add_argument("--watchdog", type=float, default=5.0)
    ap.add_argument("--expect-build", default="")
    ap.add_argument("--out", default=os.path.join(REPO, "tools", "soak_out"))
    a = ap.parse_args()
    a.duration_s = parse_duration(a.duration)
    raise SystemExit(asyncio.run(main(a)))
