# memory_budget.md — RAM budget & open62541 build flags

Status: **method + baseline defined; measured numbers TODO on hardware.**
`FreeHeap` is exposed as an OPC UA diagnostic node and via the USB `GET` command;
capture it at each step below.

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

| channels | FreeHeap (bytes) | notes |
|----------|------------------|-------|
| 0 (diag only) | TODO | boot baseline |
| 8  | TODO | |
| 16 | TODO | |
| 32 | TODO | acceptance target (must boot + browse) |

Procedure: set `channel_count` via defaults/CLI, reboot, read `Device/FreeHeap`
in UAExpert (or `GET` over USB), fill the table. If 32 channels does not fit,
lower `OPCUA_MAX_CHANNELS` and document the real ceiling here.

## Notes

- RP2350 Pico2 default flash assumed 4 MB (`PICO_FLASH_SIZE_BYTES` fallback in
  opcua_settings.c). Confirm against the actual board flash before release.
- Single-core cooperative loop (`UA_MULTITHREADING=0`); no Core1 → flash save uses
  interrupt-disable only. Revisit if A-5 adds Core1.
