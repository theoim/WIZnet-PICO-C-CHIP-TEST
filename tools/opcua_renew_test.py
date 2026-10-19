#!/usr/bin/env python3
"""Keep one secure session open across several SecureChannel renewals.

A client renews its channel at about 75 % of the token lifetime by sending a
new OPN on the same TCP connection. The server's handshake admission control
must not mistake that for a new client that has been waiting since the
connection opened - which, minutes into a session, would always look too late
and get the session dropped.

The lifetime is asked to be short (default 10 s) so renewals happen within the
test instead of every ten minutes. The test reads a node every couple of
seconds; if the connection is refused on renewal, the reads start failing.

    python tools/opcua_renew_test.py [seconds] [url]

Uses the same client certificate as opcua_concurrent_test.py.
"""

import asyncio
import sys
import time

from asyncua import Client, ua

from opcua_concurrent_test import APP_URI, CERT, DEFAULT_URL, KEY, POLICY

LIFETIME_MS = 10_000
READ_EVERY = 2.0


def ignore_server_clock() -> None:
    """Stamp each security token with the client's own clock.

    The board has no time source, so its tokens say CreatedAt = 1970. asyncua
    judges the previous token's grace period from that server timestamp against
    its local clock, decides the token expired fifty years ago, and drops the
    connection on the first message after a renewal - a client-side refusal that
    looks exactly like the server refusing the renewal. This takes the server
    clock out of the test so what remains is the server's own behaviour.
    The clock itself is a real defect and is reported separately.
    """
    from datetime import datetime, timezone
    from asyncua.common.connection import SecureConnection

    original = SecureConnection.set_channel

    def set_channel(self, params, request_type, client_nonce):
        params.SecurityToken.CreatedAt = datetime.now(timezone.utc)
        return original(self, params, request_type, client_nonce)

    SecureConnection.set_channel = set_channel


async def main(duration: float, url: str) -> int:
    client = Client(url=url, timeout=10)
    client.application_uri = APP_URI
    client.secure_channel_timeout = LIFETIME_MS
    await client.set_security_string(f"{POLICY},{CERT},{KEY}")

    await client.connect()
    print(f"connected, requested channel lifetime {LIFETIME_MS} ms, "
          f"expect a renewal every ~{LIFETIME_MS * 0.75 / 1000:.1f} s")

    started = time.perf_counter()
    reads = failures = 0
    node = client.get_node(ua.ObjectIds.Server_ServerStatus_CurrentTime)
    try:
        while time.perf_counter() - started < duration:
            try:
                await node.read_value()
                reads += 1
            except Exception as exc:                # noqa: BLE001 - reporting tool
                failures += 1
                print(f"{time.perf_counter() - started:6.1f} s  read failed: "
                      f"{type(exc).__name__}: {exc}")
                break
            await asyncio.sleep(READ_EVERY)
    finally:
        try:
            await asyncio.wait_for(client.disconnect(), timeout=5)
        except Exception:                           # noqa: BLE001
            pass

    elapsed = time.perf_counter() - started
    renewals = int(elapsed // (LIFETIME_MS * 0.75 / 1000))
    print(f"{elapsed:.1f} s, {reads} reads ok, {failures} failed, "
          f"~{renewals} renewals expected in that time")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--server-clock"]
    if "--server-clock" not in sys.argv:
        ignore_server_clock()
    secs = float(args[0]) if len(args) > 0 else 40.0
    target = args[1] if len(args) > 1 else DEFAULT_URL
    sys.exit(asyncio.run(main(secs, target)))
