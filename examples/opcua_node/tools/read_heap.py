#!/usr/bin/env python3
"""단일 세션으로 접속해 Diag.FreeHeap 과 채널 수를 읽는다.

memory_budget.md 의 채널 스윕(3/8/16/32)에서 이미지를 바꿔 플래시할 때마다
같은 조건으로 힙을 재기 위한 도구다. 세션 하나, 구독 없음 — 세션 비용이
섞이지 않도록 session_scaling_test.py 와 조건을 일부러 다르게 한다.

usage: python read_heap.py [endpoint]
"""
import asyncio
import sys

from asyncua import Client

ENDPOINT = sys.argv[1] if len(sys.argv) > 1 else "opc.tcp://192.168.11.2:4840"

# RP2040(W55RP20) 링커 실측값. __StackLimit - __bss_end__.
HEAP_SPAN = 197880
STATIC_RAM = 64264
TOTAL_SRAM = 264 * 1024


async def main() -> int:
    async with Client(url=ENDPOINT) as client:
        ns = await client.get_namespace_index("urn:wiznet:opcua-node")

        free = await client.get_node(f"ns={ns};s=Diag.FreeHeap").read_value()
        name = await client.get_node(f"ns={ns};s=Diag.DeviceName").read_value()

        # Channels 폴더를 브라우즈해서 실제로 생성된 채널 노드 수를 센다.
        # 설정값이 아니라 주소공간에 올라간 개수를 봐야 AddNode 실패를 놓치지 않는다.
        root = await client.nodes.objects.get_child([f"{ns}:{name}", f"{ns}:Channels"])
        channels = await root.get_children()

    used = HEAP_SPAN - free
    total = STATIC_RAM + used

    print(f"endpoint   : {ENDPOINT}")
    print(f"device     : {name}")
    print(f"channels   : {len(channels)} node(s) under Channels")
    print(f"FreeHeap   : {free:,} B")
    print(f"heap used  : {used:,} B  ({used / 1024:.1f} KB)")
    print(f"static+heap: {total:,} B  ({total / 1024:.1f} KB, "
          f"{total / TOTAL_SRAM * 100:.1f} % of 264 KB)")
    print()
    print(f"| {len(channels)} | {free:,} | {used:,} B ({used / 1024:.1f} KB) | |")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
