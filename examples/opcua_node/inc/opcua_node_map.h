#ifndef OPCUA_NODE_MAP_H
#define OPCUA_NODE_MAP_H

/*
 * Numeric NodeId allocation for the dynamically generated address space.
 *
 * Layout (namespace = urn:wiznet:opcua-node):
 *   Objects
 *     <DeviceName>                        ns=1;i=OPCUA_ID_ROOT
 *       Device                            ns=1;i=OPCUA_ID_DEVICE
 *         DeviceName/IPAddress/FwVersion/Uptime_s/FreeHeap/
 *         RebootCount/RebootReason/LinkStatus/RawFrame/
 *         Usb / Uart / Modbus diagnostics  ns=1;s="Diag.*"
 *       Channels                          ns=1;i=OPCUA_ID_CHANNELS
 *         <channel name>                  ns=1;s="Channels/<name>"  (DataSource)
 *           EngineeringUnits              ns=1;i=OPCUA_ID_PROP_BASE + idx*4 + 0
 *           EURange                       ns=1;i=OPCUA_ID_PROP_BASE + idx*4 + 1
 *
 * Channel read-callback node_context = channel index (0..count-1).
 * Diagnostic read-callback node_context = OPCUA_DIAG_CTX_BASE + kind.
 */

#define OPCUA_NODE_NS_URI       "urn:wiznet:opcua-node"

#define OPCUA_ID_ROOT           1000u
#define OPCUA_ID_DEVICE         1001u
#define OPCUA_ID_CHANNELS       1002u

#define OPCUA_ID_DIAG_BASE      3000u   /* diagnostic variable numeric ids     */
#define OPCUA_ID_PROP_BASE      5000u   /* per-channel property numeric ids     */

/* node_context tag separating diagnostics from channel indices. Channels are
 * always < OPCUA_MAX_CHANNELS (32), so 0x1000 is a safe, non-overlapping base. */
#define OPCUA_DIAG_CTX_BASE     0x1000u

/* Diagnostic kinds (offset from OPCUA_DIAG_CTX_BASE in node_context). */
typedef enum {
    OPCUA_DIAG_DEVICE_NAME = 0,
    OPCUA_DIAG_IP,
    OPCUA_DIAG_FW_VERSION,
    OPCUA_DIAG_UPTIME_S,
    OPCUA_DIAG_FREE_HEAP,
    OPCUA_DIAG_REBOOT_COUNT,
    OPCUA_DIAG_REBOOT_REASON,
    OPCUA_DIAG_LINK_STATUS,
    OPCUA_DIAG_RAW_FRAME,
    OPCUA_DIAG_USB_FRAMES,
    OPCUA_DIAG_USB_ERRORS,
    OPCUA_DIAG_UART_FRAMES,
    OPCUA_DIAG_UART_ERRORS,
    OPCUA_DIAG_MODBUS_POLLS,
    OPCUA_DIAG_MODBUS_TIMEOUTS,
    OPCUA_DIAG__COUNT
} OpcUaDiagKind;

#endif /* OPCUA_NODE_MAP_H */
