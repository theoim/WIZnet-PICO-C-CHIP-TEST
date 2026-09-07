# node_map.md — Channel schema & OPC UA address space

Status: **A-1 implemented** (dynamic address space from settings). A-2/A-3 fill the
channel input sources; this doc describes the model already in code.

## Namespace

- URI: `urn:wiznet:opcua-node` (namespace index resolved at runtime, typically `ns=2`).
- Endpoint URL: built at boot from the configured IPv4 → `opc.tcp://<ip>:4840`.

## Address space

```
Objects
  <DeviceName>                         ns;i=1000   (settings.device_name)
    Device                             ns;i=1001
      DeviceName        String         ns;s="Diag.DeviceName"
      IPAddress         String         ns;s="Diag.IPAddress"
      FwVersion         String         ns;s="Diag.FwVersion"
      Uptime_s          UInt32         ns;s="Diag.Uptime_s"
      FreeHeap          UInt32         ns;s="Diag.FreeHeap"
      RebootCount       UInt32         ns;s="Diag.RebootCount"
      RebootReason      String         ns;s="Diag.RebootReason"    (power|watchdog|user)
      LinkStatus        String         ns;s="Diag.LinkStatus"      (up|down)
      RawFrame          String         ns;s="Diag.RawFrame"        (last input frame)
      UsbFrameCount     UInt32         ns;s="Diag.UsbFrameCount"
      UsbErrorCount     UInt32         ns;s="Diag.UsbErrorCount"
      UartFrameCount    UInt32         ns;s="Diag.UartFrameCount"
      UartErrorCount    UInt32         ns;s="Diag.UartErrorCount"
      ModbusPollCount   UInt32         ns;s="Diag.ModbusPollCount"
      ModbusTimeoutCount UInt32        ns;s="Diag.ModbusTimeoutCount"
    Channels                           ns;i=1002
      <name>            <datatype>     ns;s="Channels/<name>"      (DataSource, read-only)
        EngineeringUnits String        ns;i=5000+idx*4+0           (Property, if unit set)
        EURange          Double[2]      ns;i=5000+idx*4+1           (Property, [eu_low, eu_high])
```

All variables are read-only (`UA_ACCESSLEVELMASK_READ`) and served through a single
DataSource read callback (`opcua_read_value`) — pull model, no per-node value push.
The callback's `node_context` encodes the source: channel index `0..N-1` for channels,
`0x1000 + kind` for diagnostics.

## Channel definition schema (`channel_def_t`, 80 bytes)

| Field | Type | Notes |
|-------|------|-------|
| id | uint16 | stable channel id |
| name | char[32] | browse/display name, must be unique |
| source | uint8 | 0=USB, 1=UART, 2=MODBUS |
| datatype | uint8 | 0=Float, 1=Int32, 2=Bool, 3=String |
| unit | char[16] | EngineeringUnits text |
| scale | float | engineering = raw*scale + offset |
| offset | float | |
| eu_low / eu_high | float | EURange property (extension beyond original brief) |
| enabled | uint8 | 0 = defined but not instantiated |
| slave_addr | uint8 | Modbus only |
| function_code | uint8 | 3/4/1/2 |
| word_order | uint8 | 0=big (ABCD), 1=little (CDAB) |
| register_addr | uint16 | |
| register_count | uint16 | 1=16-bit, 2=32-bit |
| poll_ms | uint16 | 0 = use modbus global default (extension) |

## Datatype → OPC UA mapping

| datatype | Variant type | value source |
|----------|-------------|--------------|
| Float | Float | scaled double → (float) |
| Int32 | Int32 | scaled double → rounded int32 |
| Bool | Boolean | bool_value |
| String | String | str[] |

Bad quality: when an input driver marks a channel via `data_table_set_status()`
(e.g. `UA_STATUSCODE_BADCOMMUNICATIONERROR` on Modbus timeout), the read callback
returns that StatusCode with no value — quality is exposed, stale values are not held.

## EURange / EngineeringUnits decision

Full DataAccess `AnalogItemType` + `EUInformation` needs NS0 nodes that the MINIMAL
namespace-zero build does not ship. To stay dependency-free in Phase A, units are
exposed as a plain String property (`EngineeringUnits`) and range as a `Double[2]`
property (`EURange`). Promoting to full AnalogItem is a Phase B/C task gated on
enabling a richer NS0 (see docs/memory_budget.md for the amalgamation flags).
