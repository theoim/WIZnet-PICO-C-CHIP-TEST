#!/usr/bin/env python3
"""
What happens when several clients secure a channel at the same moment.

The arithmetic says this is the constraint on the product. The server is a
single-core cooperative loop and an RSA private key operation cannot be
interrupted, so handshakes are processed one after another: the measured
1,229 ms of arithmetic per OpenSecureChannel puts a fourth simultaneous client
at roughly 4.9 s against a 5 s client timeout. A plant restart reconnecting
every device at once is exactly that case.

That figure is a floor derived from primitives, and the first live handshake
came in under it (936 ms against 1,229 ms, because the client's OPN fit in one
OAEP block rather than two). So the serial number here is not a confirmation of
the model - it replaces it.

Run it against a server built with -DOPCUA_ENABLE_ENCRYPTION=ON. The client
certificate comes from tools/make_client_cert.sh.

Usage:
    python tools/opcua_concurrent_test.py [N] [opc.tcp://host:port]

    N defaults to 4, which is the server's maxSecureChannels.
"""

import asyncio
import os
import statistics
import sys
import time

from asyncua import Client

HERE = os.path.dirname(os.path.abspath(__file__))
CERT_DIR = os.path.join(HERE, "interop_certs")
CERT = os.path.join(CERT_DIR, "client_cert.der")
KEY = os.path.join(CERT_DIR, "client_key.pem")

# Must equal the subjectAltName URI in the client certificate, or the server
# rejects it with BadCertificateUriInvalid - the same rule as the server's own.
APP_URI = "urn:wiznet:interop-client"

DEFAULT_URL = "opc.tcp://192.168.11.2:4840"
POLICY = "Basic256Sha256,SignAndEncrypt"


async def one_client(index: int, url: str, barrier: asyncio.Barrier) -> dict:
    """Connect, time it, disconnect. Waits on the barrier so all start together."""
    client = Client(url=url)
    client.application_uri = APP_URI
    await client.set_security_string(f"{POLICY},{CERT},{KEY}")

    # Without this the clients start as the event loop gets round to them, which
    # measures scheduling rather than the server's serialisation.
    await barrier.wait()

    started = time.perf_counter()
    try:
        await client.connect()
        elapsed = time.perf_counter() - started
        result = {"index": index, "ok": True, "seconds": elapsed, "error": None}
        await client.disconnect()
    except Exception as exc:                        # noqa: BLE001 - reporting tool
        elapsed = time.perf_counter() - started
        result = {"index": index, "ok": False, "seconds": elapsed,
                  "error": f"{type(exc).__name__}: {exc}"}
    return result


async def main(count: int, url: str) -> int:
    for path in (CERT, KEY):
        if not os.path.exists(path):
            print(f"missing {path}\nrun tools/make_client_cert.sh first")
            return 1

    print(f"{count} clients, {POLICY}, {url}\n")

    barrier = asyncio.Barrier(count)
    started = time.perf_counter()
    results = await asyncio.gather(
        *(one_client(i, url, barrier) for i in range(count))
    )
    wall = time.perf_counter() - started

    results.sort(key=lambda r: r["seconds"])

    print(f"{'client':>6}  {'result':<8} {'seconds':>8}  detail")
    print("-" * 72)
    for r in results:
        detail = r["error"] or ""
        print(f"{r['index']:>6}  {'ok' if r['ok'] else 'FAILED':<8} "
              f"{r['seconds']:>8.3f}  {detail}")

    ok = [r["seconds"] for r in results if r["ok"]]
    print()
    print(f"wall clock for all {count}: {wall:.3f} s")

    if ok:
        print(f"first  completed at {min(ok):.3f} s")
        print(f"last   completed at {max(ok):.3f} s")
        if len(ok) > 1:
            print(f"median                {statistics.median(ok):.3f} s")
            gaps = [b - a for a, b in zip(ok, ok[1:])]
            print(f"gap between finishes  "
                  f"{min(gaps):.3f} .. {max(gaps):.3f} s")
            print()
            print("Even spacing means the handshakes were serialised and each")
            print("gap is one handshake. Clustered finishes would mean they")
            print("overlapped, which this server cannot do - so flat gaps are")
            print("the expected shape, and their size is the per-handshake cost.")

    failed = [r for r in results if not r["ok"]]
    if failed:
        print(f"\n{len(failed)} of {count} failed. A timeout here is the result,")
        print("not a broken test: it is what a client sees on a plant restart.")

    # The number the product decision needs.
    if ok:
        print(f"\nworst case against the 5 s client timeout: "
              f"{5.0 - max(ok):+.3f} s of margin")

    return 0


async def sequential(count: int, url: str) -> int:
    """Connect one at a time and hold them all open.

    A different question from the one above, and the two must not be answered
    with one number. The concurrent test measures how many RSA handshakes can
    start together inside a connection-time target; this measures how many
    secure channels and sessions the server can carry once they exist. A device
    that can hold four clients but only admit two at a time is a normal shape,
    and collapsing that into "maximum secure connections: 2" would halve the
    stated capacity for a reason that only applies during a reconnect storm.
    """
    for path in (CERT, KEY):
        if not os.path.exists(path):
            print(f"missing {path}\nrun tools/make_client_cert.sh first")
            return 1

    print(f"{count} clients one at a time, held open, {POLICY}, {url}\n")

    clients = []
    held = []
    try:
        for i in range(count):
            client = Client(url=url)
            client.application_uri = APP_URI
            await client.set_security_string(f"{POLICY},{CERT},{KEY}")

            started = time.perf_counter()
            try:
                await client.connect()
            except Exception as exc:                # noqa: BLE001
                print(f"client {i}: FAILED after "
                      f"{time.perf_counter() - started:.3f} s - "
                      f"{type(exc).__name__}: {exc}")
                break

            elapsed = time.perf_counter() - started
            clients.append(client)
            print(f"client {i}: connected in {elapsed:.3f} s "
                  f"({len(clients)} held)")

        print()
        if not clients:
            print("nothing connected")
            return 1

        # Holding a channel is not the same as it still working. Each one reads
        # a node, so a channel that was silently dropped shows up here rather
        # than being counted as held.
        print("verifying every held session still answers")
        for i, client in enumerate(clients):
            try:
                node = client.get_node("i=2258")      # Server_ServerStatus_CurrentTime
                value = await node.read_value()
                held.append(i)
                print(f"  client {i}: read ok  ({value})")
            except Exception as exc:                 # noqa: BLE001
                print(f"  client {i}: read FAILED - {type(exc).__name__}: {exc}")

        print()
        print(f"held and working: {len(held)} of {count}")
        if len(held) == count:
            print("Established secure sessions and concurrent handshake")
            print("capacity are separate numbers, and this is the first.")
    finally:
        for client in clients:
            try:
                await client.disconnect()
            except Exception:                        # noqa: BLE001
                pass

    return 0 if len(held) == count else 1


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--sequential"]
    seq = "--sequential" in sys.argv

    n = int(args[0]) if args else 4
    target = args[1] if len(args) > 1 else DEFAULT_URL

    runner = sequential(n, target) if seq else main(n, target)
    raise SystemExit(asyncio.run(runner))
