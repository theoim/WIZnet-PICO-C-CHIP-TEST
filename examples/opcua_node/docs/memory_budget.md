# memory_budget.md — RAM budget & open62541 build flags

Status: **W55RP20 (RP2040) is the current target. Channel sweep (3/8/16/32) and
1..4-session cost both measured on hardware 2026-09-10. The acceptance case --
32 channels with 4 concurrent sessions -- runs at 55.5 % of SRAM.**
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

## RAM per channel (measured 2026-09-10, W55RP20 / RP2040)

Runtime `channel_value_t` = ~120 B x 32 = ~3.8 KB static (data_table), always
allocated regardless of `channel_count`. The figure below is the open62541 heap
cost: one Variable node + EngineeringUnits + EURange properties + references.

Method: `-DOPCUA_DEFAULT_CHANNEL_COUNT=N`, reflash, then
`tools/read_heap.py` (one session, no subscription). The script also browses the
`Channels` folder and counts the nodes actually created, so a silent AddNode
failure cannot be mistaken for a low heap reading.

| channels | FreeHeap (B) | heap allocated (B) | per channel | notes |
|---------:|-------------:|-------------------:|------------:|-------|
| 3 (defaults) | 151,488 | 46,392 (45.3 KB) | baseline | |
| 8  | 146,312 | 51,568 (50.4 KB) | 1,035 B | |
| 16 | 138,024 | 59,856 (58.5 KB) | 1,036 B | |
| 32 | **119,968** | **77,912 (76.1 KB)** | 1,129 B | all 32 nodes created |

**All 32 channel nodes are created and readable.** Free heap at 32 channels is
119,968 B (117.2 KB); total usage is 64,264 + 77,912 = 142,176 B
= **138.8 KB, 52.6 % of the 264 KB SRAM**. RAM is not the ceiling at 32 channels.

Read the 32-channel row from a **freshly booted** device. A first reading of
116,368 B was taken on a board that still held two orphaned sessions from earlier
failed connects (~2.3 KB each, see the session-reaping note below); it made the
per-channel cost look like 1,353 B. The clean reading is 119,968 B and the cost
1,129 B/channel, close to the flat 1,035 B seen up to 16 channels. The remaining
~90 B/channel step over the 16-to-32 span is unexplained; a nodestore hash-map
growth is plausible (3 nodes per channel means 48 nodes at 16 and 96 at 32) but
is not confirmed in the source.

For comparison, CSA's STM32H735 reference reports ~900 B/node at 300 nodes.
Our 1,035 B/channel covers three nodes, so the per-node cost is well under that.

## Browse response ceiling (FIXED 2026-09-10)

At 32 channels an unrestricted `Browse` of the `Channels` folder used to **close
the connection**. Reads still worked and the address space was intact. Splitting
the same browse into batches worked, which located the fault in the send path
rather than in node handling:

| `RequestedMaxReferencesPerNode` | before fix |
|---|---|
| 4 / 8 / 16 | OK, 32 refs over 8 / 4 / 2 rounds |
| 0 (unlimited) | connection closed |

Measured response sizes (client-side encoding of the returned
`ReferenceDescription`s): ~70 B per ref, so 16 refs = 1,125 B and 32 refs =
2,250 B, about 2,349 B on the wire. The threshold sat on the **W5500 socket TX
buffer of 2,048 B**, not on the 8,192 B open62541 send buffer.

### Root cause

`WIZ_EL_DEBUG=1` printed the deciding line:

```
[el] send s0 FAIL rc=0 sent=2048/2349 sr=0x17
```

Return code 0 with the socket still in `0x17` (`SOCK_ESTABLISHED`). In ioLibrary,
**0 is `SOCK_BUSY`** (`Ethernet/socket.h:96`), returned from `send()` at
`Ethernet/socket.c:548` when the previous `SEND` command has not raised
`Sn_IR_SENDOK` yet. It means "call me again", not "failed". Real errors are
negative (`SOCKERR_TIMEOUT`, `SOCKERR_SOCKSTATUS`).

`wiz_send_all()` in `port/open62541/open62541_wiznet_eventloop.c` tested
`if(sent <= 0)` and treated the busy return as a dead connection, closing the
socket mid-message.

This was never specific to Browse or to 32 channels. **Any response larger than
the 2 KB socket TX buffer hit it** -- large Reads and Publish responses carrying
many notifications included. 32 channels merely produced the first message big
enough to cross the line.

### Fix

`wiz_send_all()` now fails only on a negative return, retries on 0, and bounds
the *stall* (time with no progress) at 5 s rather than the whole transfer -- any
successful chunk resets the deadline. The W5500 raises its own retransmission
timeout at roughly 1.8 s (RTR/RCR defaults) and returns a negative code first, so
the 5 s guard is a backstop, not the normal exit.

Verified on hardware 2026-09-10 with the 32-channel image: unrestricted Browse
returns all 32 references in one response, no `FAIL` or `STALL` on the console,
and the 4-session scaling test still passes unchanged.

### Related open item: socket TX buffer sizing

The W5500 has 16 KB of TX buffer for 8 sockets and `wiznet_network.c:144` uses
`wizchip_initialize()`, which splits it evenly at 2 KB each. Giving the four
OPC UA sockets 4 KB each (`{4,4,4,4,0,0,0,0}`) would let a 2.3 KB response leave
in a single chunk, at the cost of leaving nothing for a future HTTP config page
or DHCP socket. That is a product decision, not a correctness one -- the send
path is correct at 2 KB now. Not changed.

Note also that `opcua_server.c:497` justifies `maxNotificationsPerPublish = 100`
with "a response that fits the 6KB TX buffer". That comment describes the W6300
socket budget and is wrong for W55RP20, whose TX buffer is 2 KB. The limit needs
re-deriving for this board.

## Per-session cost (measured 2026-09-10, W55RP20 / RP2040)

Build: 4 listening sockets, W5500 default socket buffers, `WIZ_EL_DEBUG=1` instrumentation.
Each measurement had one Subscription (500 ms) + one MonitoredItem (50 ms) per session,
opened by `tools/session_scaling_test.py`. Three channels configured.

| sessions | FreeHeap (B) | heap allocated (B) | delta |
|---------:|-------------:|-------------------:|------:|
| 1 | 150,736 | 47,144 | baseline |
| 2 | 148,440 | 49,440 | -2,296 |
| 3 | 146,072 | 51,808 | -2,368 |
| 4 | **143,704** | **54,176** | -2,368 |

**Cost per additional session: 2,344 B average (2.29 KB) at 3 channels.**

Repeated at **32 channels** on 2026-09-10 after the send fix:

| sessions | FreeHeap (B) | heap allocated (B) | delta |
|---------:|-------------:|-------------------:|------:|
| 1 | 119,216 | 78,664 | baseline |
| 2 | 116,848 | 81,032 | -2,368 |
| 3 | 114,472 | 83,408 | -2,376 |
| 4 | **112,048** | **85,832** | -2,424 |

2,389 B average -- within 2 % of the 3-channel figure, so session cost does not
depend on address-space size.

**Acceptance case: 32 channels + 4 sessions = 64,264 static + 85,832 heap
= 150,096 B (146.6 KB) = 55.5 % of the 264 KB SRAM.** Free heap 112,048 B
(109.4 KB). This is the number to quote for Phase A on W55RP20.
RP2350/W6300 measured 2,445 B on 2026-09-03 -- the two platforms agree within 4 %,
so the figure is a property of open62541, not of the board.

### The 4th-session trap (resolved 2026-09-10)

Earlier runs appeared to cap at **3** sessions. The cause was not the chip and not
open62541: a UAExpert session left connected on the workstation was holding one of
the four sockets. Closing the UAExpert *window* is not enough -- the session survives
until its 600 s timeout, so the socket stays `SOCK_ESTABLISHED`.

Confirmed with the `WIZ_EL_DEBUG` eventloop trace. Before starting any session test,
all four sockets must read `0x14` (`SOCK_LISTEN`):

```
[el] n=4  s0:sr=0x14  s1:sr=0x14  s2:sr=0x14  s3:sr=0x14  listenAnn=1
```

With that precondition met, all four sessions connect and the 5th is refused:

```
[el] n=4  s0:sr=0x17  s1:sr=0x17  s2:sr=0x17  s3:sr=0x17  listenAnn=1
[5] CONNECT FAIL    2.02s  ConnectionRefusedError
```

A 5th connection is refused at the transport layer with an explicit RST rather than
hanging, and closing one session frees a slot immediately (0.02 s on retry).
The refusal itself takes about 2 s, which is still unexplained -- likely the first
SYN is dropped and the RST arrives on the retransmit. Confirming that needs a packet
capture. The same 2 s figure was measured on W6300/RP2350, so it is not W5500-specific.

**Memory is not the constraint.** The socket count is, and 4 is the configured
`OPCUA_MAX_SOCKETS`. Phase B security has ample heap room (140 KB free at 4 sessions).

## Measured SRAM breakdown (W55RP20 / RP2040, 3 channels, 2026-09-10)

Heap span comes from the linker: `__StackLimit - __bss_end__`.

```
__bss_end__   0x2000FB08
__StackLimit  0x20040000        heap span 197,880 B (193.2 KB)
__StackTop    0x20042000        stack       8,192 B (8.0 KB)
```

| region | bytes | KB | note |
|---|---:|---:|---|
| static (`.data` + `.bss` + vector table) | 64,264 | 62.8 | `UA_TYPES` alone is 27.3 KB of this |
| heap -- allocated (1 session) | 47,144 | 46.0 | open62541 runtime, 3 channels |
| heap -- allocated (4 sessions) | 54,176 | 52.9 | |
| heap -- free (4 sessions) | 143,704 | 140.3 | |
| stack region | 8,192 | 8.0 | reserved, high-water not measured |
| **total** | **270,336** | **264.0** | matches RP2040 SRAM exactly |

**Usage at 1 session = 111,408 B (108.8 KB) = 41.2 % of SRAM.**
**Usage at 4 sessions = 118,440 B (115.7 KB) = 43.8 % of SRAM.**

RP2040 static RAM (62.8 KB) is within 0.6 KB of the RP2350 build (63.4 KB), so the
one-chip W55RP20 costs essentially nothing extra in static footprint. The real
difference is the heap ceiling: 193.2 KB here versus 448.6 KB on RP2350.

Caveats:
- Stack high-water is NOT measured. The 8,192 B is the reserved region, not usage.
- An earlier revision of this file reported "42.4 KB heap used" for the W55RP20.
  That figure divided by the RP2350 heap span and was wrong; the corrected value is
  46.0 KB at 1 session.
- These readings are from a `WIZ_EL_DEBUG=1` build. The trace adds only `printf`
  calls and no heap allocation, but re-confirm once the flag is back to 0.

## Notes

- W55RP20 has 2 MB flash; `PICO_BOARD=pico` supplies `PICO_FLASH_SIZE_BYTES`
  correctly, so the 4 MB fallback in opcua_settings.c is not used on this target.
  Re-confirm if the board is switched back to a Pico2 variant.
- Single-core cooperative loop (`UA_MULTITHREADING=0`); no Core1 -> flash save uses
  interrupt-disable only. Revisit if A-5 adds Core1.
- open62541 tries to create a session diagnostics object per session and fails with
  `BadParentNodeIdInvalid` (parent `i=3706`, `SessionsDiagnosticsSummary`, absent from
  NS0 MINIMAL). Harmless but noisy; two wasted AddNode calls per session.

## Open: sessions are not reaped after an abrupt disconnect

Sockets come back in 0.02 s, but sessions do not. A client whose TCP connection
dies without a CloseSession leaves the session allocated until
`maxSessionTimeout` (currently 10 min, `opcua_server.c:496`) expires. With
`maxSessions = 4`, four such orphans lock the server out entirely:

```
ServiceFault (BadTooManySessions) in response to CreateSessionRequest
```

Hit repeatedly on 2026-09-10 while iterating the browse tests. Each orphan also
holds its ~2.4 KB, which silently skews FreeHeap readings -- the first
32-channel measurement (116,368 B) was 3.6 KB low for exactly this reason.

Two things to decide, not yet done:
- whether a much shorter `maxSessionTimeout` is right for a 4-socket device
- whether losing the transport connection should drop the session immediately
  rather than waiting for the timeout

Until then: **reboot the board between test runs, and read FreeHeap only from a
freshly booted device.**
