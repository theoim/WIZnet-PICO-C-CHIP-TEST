#!/usr/bin/env python3
"""
Ask the server what it offers, and say what a client sees.

This is the first row of the interoperability matrix and the cheapest one:
GetEndpoints needs no client certificate and no trust relationship, so it
separates three failures that otherwise look alike - the server is unreachable,
the server answers but offers nothing in common, or the handshake itself fails
later on.

It also settles a question the serial log cannot. The server builds its
DiscoveryUrl from a socket bound to every address, so it logs
opc.tcp://0.0.0.0:4840. What matters is the endpointUrl inside each
EndpointDescription, because that is what a client reconnects to - and whether
open62541 rewrites it per request is easier to read off a live server than out
of the source.

Usage:  python tools/opcua_interop_probe.py [opc.tcp://host:port]
"""

import asyncio
import sys

from asyncua import Client, ua

DEFAULT_URL = "opc.tcp://192.168.11.2:4840"

MODE = {
    ua.MessageSecurityMode.None_: "None",
    ua.MessageSecurityMode.Sign: "Sign",
    ua.MessageSecurityMode.SignAndEncrypt: "SignAndEncrypt",
}


def short(policy_uri: str) -> str:
    """The part of the policy URI that identifies it."""
    return policy_uri.rsplit("#", 1)[-1] if policy_uri else "(none)"


async def main(url: str) -> int:
    print(f"probing {url}\n")

    client = Client(url=url)
    try:
        endpoints = await client.connect_and_get_server_endpoints()
    except Exception as exc:                       # noqa: BLE001 - report, not handle
        print(f"GetEndpoints FAILED: {type(exc).__name__}: {exc}")
        print("\nNothing below this point can be tested until that works.")
        return 1

    print(f"{len(endpoints)} endpoints\n")
    print(f"{'policy':<18} {'mode':<15} {'level':>5}  endpointUrl")
    print("-" * 78)

    urls = set()
    for ep in endpoints:
        mode = MODE.get(ep.SecurityMode, str(ep.SecurityMode))
        print(f"{short(ep.SecurityPolicyUri):<18} {mode:<15} "
              f"{ep.SecurityLevel:>5}  {ep.EndpointUrl}")
        urls.add(ep.EndpointUrl)

    print()

    # The thing this probe exists to answer.
    if any("0.0.0.0" in u for u in urls):
        print("endpointUrl carries 0.0.0.0 - a client that reconnects to the")
        print("URL the server returns will not get there. Expect this to look")
        print("like a client bug during the matrix; it is ours.")
    else:
        print("endpointUrl is a reachable address - the 0.0.0.0 in the server's")
        print("own log is the DiscoveryUrl only, and does not reach clients.")

    # Application identity: a mismatch here rejects every certificate later.
    desc = endpoints[0].Server
    print(f"\napplicationUri : {desc.ApplicationUri}")
    print(f"productUri     : {desc.ProductUri}")
    print("the certificates' subjectAltName URI must equal applicationUri,")
    print("or secure connections fail with BadCertificateUriInvalid")

    return 0


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL
    raise SystemExit(asyncio.run(main(target)))
