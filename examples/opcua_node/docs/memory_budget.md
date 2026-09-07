# memory_budget.md — RAM budget & open62541 build flags

Status: **3-channel and per-session measurements done on hardware (2026-09-03). Channel 0 / 8 / 16 / 32 sweep still TODO.**
`FreeHeap` is exposed as an OPC UA diagnostic node (`ns=2;s=Diag.FreeHeap`,
path `WIZnet-OPCUA-Node/Device/FreeHeap`).
**It is NOT printed by the USB `GET` command** -- `usb_stdio_print_state()`
only prints frame counters and channel values. UAExpert (or another OPC UA
client) is required for every reading below.

## open62541 amalgamation flags (baseline)

Generated automatically by `port/open62541/CMakeLists.txt` from `libraries/open62541`.
These flags are the Phase A baseline; Phase B (security) and Phase C (PubSub) will
re-tune them, so record any change here with before/after FreeHeap.

```
UA_ARCHITECTURE=none            (custom WIZnet ioLibrary eventloop shim)
UA_NAMESPACE_ZERO=MINIMAL
UA_ENABLE_SUBSCRIPTIONS=ON
UA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF
UA_ENABLE_METHODCALLS=ON
UA_ENABLE_NODEMANAGEMENT=ON     (required for A-4 hot reload / deleteNode)
UA_ENABLE_HISTORIZING=OFF
UA_ENABLE_PUBSUB=OFF
UA_ENABLE_DISCOVERY=OFF
UA_MULTITHREADING=0
```

Server buffers: `UA_ServerConfig_setMinimalCustomBuffer(port, NULL, 8192, 8192)`
+ eventloop RX buffer 8192 (`open62541_wiznet_eventloop.c`).

## Settings blob (flash, not RAM)

`opcua_settings_t` = **2816 bytes**, one flash sector (last sector).
- fixed header + network + auth + Phase B reserve + reboot record: 176 B
- `channel_def_t[32]` = 80 × 32 = 2560 B
- tail pad + crc32: 80 B

## RAM per channel (measure)

Runtime `channel_value_t` = ~120 B × 32 = ~3.8 KB static (data_table).
open62541 per-node cost (object + variable + 2 properties + references) is the
figure to measure: record FreeHeap at 0 / 8 / 16 / 32 channels.

| channels | FreeHeap (bytes) | heap allocated | notes |
|----------|-----------------:|---------------:|-------|
| 0 (diag only) | TODO | | boot baseline |
| **3 (defaults)** | **415,536 / 415,560** | **43,832 B (42.8 KB)** | **measured 2026-09-03**, two readings 24 B apart |
| 8  | TODO | | |
| 16 | TODO | | |
| 32 | TODO | | acceptance target (must boot + browse) |

Two consecutive readings differed by 24 B, so the server is stable at idle --
no fast leak. A soak test is still needed for the A-5 trend.

Procedure: set `channel_count` via defaults/CLI, reboot, connect UAExpert to
`opc.tcp://192.168.11.2:4840` (SecurityPolicy None, Anonymous), read
`Device/FreeHeap`, fill the table. If 32 channels does not fit,
lower `OPCUA_MAX_CHANNELS` and document the real ceiling here.

## Per-session cost (measured 2026-09-03, multi-socket build)

Build: 4 listening sockets, `memsize {4,4,4,4,8,2,2,0}`, D-7 limits applied.
Each measurement had one Subscription + one MonitoredItem per session.

All four points are **measured**, not extrapolated. Sessions 2-4 were opened
with an `asyncua` script (`tools/session_scaling_test.py`) while UAExpert held
session 1; each script session created one Subscription + one MonitoredItem,
matching what UAExpert does. Two independent runs, values averaged.

| sessions | FreeHeap (B) | heap allocated (B) | delta |
|---------:|-------------:|-------------------:|------:|
| 1 | 414,224 | 45,156 | baseline |
| 2 | 411,680 | 47,700 | -2,544 |
| 3 | 409,368 | 50,012 | -2,312 |
| 4 | **406,888** | **52,492** | -2,480 |

**Cost per additional session: 2,445 B average (2.39 KB).**

Lower than expected — the server buffers are configured at 8192 each, but
open62541 does not pre-allocate them per SecureChannel.

The earlier 2-session extrapolation predicted 406,580 at 4 sessions; the
measured value is 406,888, off by 308 B. The linear model holds.

Total RAM at 4 sessions (measured): static 63.4 KB + heap 51.3 KB
= **114.6 KB, 22.1 % of the 520 KB SRAM.**

A 5th connection is refused at the transport layer -- see
`product_direction.md` 8-5.

**Memory is not the constraint.** The transport socket count was, and that is
now resolved (4 sockets). Phase B security has ample room.

Run-to-run spread at the same session count was under 130 B, so the server is
stable at idle. A soak test is still required for the A-5 leak trend.

## Measured SRAM breakdown (3 channels, 2026-09-03)

Heap span comes from the linker: `__StackLimit - __bss_end__`.

```
__bss_end__   0x2000FD8C
__StackLimit  0x20080000        heap span 459,380 B (448.6 KB)
__StackTop    0x20082000        stack      8,192 B (8.0 KB)
```

| region | bytes | KB | note |
|---|---:|---:|---|
| static (`.data` + `.bss` + vector table) | 64,908 | 63.4 | `UA_TYPES` alone is 27.3 KB of this |
| heap -- allocated | 43,832 | 42.8 | open62541 runtime, 3 channels, 1 session |
| heap -- free | 415,548 | 405.8 | |
| stack region | 8,192 | 8.0 | reserved, high-water not measured |
| **total** | **532,480** | **520.0** | matches RP2350 SRAM exactly |

**Actual usage = static + heap allocated = 108,740 B (106.2 KB) = 20.4 % of SRAM.**
Headroom 413.8 KB.

Caveats:
- One UAExpert session connected. This breakdown predates the multi-socket
  change; for the 2 / 4 session figures see the per-session table above.
- `EURange` failed to be added on all 3 channels at this measurement, so the
  real per-channel cost is slightly higher than this reading implies.
- Stack high-water is NOT measured. The 8,192 B is the reserved region, not usage.

## Notes

- RP2350 Pico2 default flash assumed 4 MB (`PICO_FLASH_SIZE_BYTES` fallback in
  opcua_settings.c). Confirm against the actual board flash before release.
- Single-core cooperative loop (`UA_MULTITHREADING=0`); no Core1 → flash save uses
  interrupt-disable only. Revisit if A-5 adds Core1.
