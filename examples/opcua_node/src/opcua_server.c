#include "opcua_server.h"

#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "data_table.h"
#include "opcua_node_map.h"
#include "opcua_tcp_probe.h"
#include "pico/stdlib.h"
#include "wiznet_network.h"
#include "wiznet_eventloop_config.h"

#ifndef OPCUA_ENABLE
#define OPCUA_ENABLE 0
#endif

#ifndef OPCUA_TCP_PROBE_ENABLE
#define OPCUA_TCP_PROBE_ENABLE 0
#endif

static OpcUaServerStatus s_status;
static const opcua_settings_t *s_cfg;
static char s_endpoint[48];

static const char *state_name(OpcUaServerState state) {
    switch(state) {
    case OPCUA_SERVER_STATE_DISABLED: return "DISABLED";
    case OPCUA_SERVER_STATE_READY:    return "READY";
    case OPCUA_SERVER_STATE_RUNNING:  return "RUNNING";
    case OPCUA_SERVER_STATE_ERROR:    return "ERROR";
    default:                          return "UNKNOWN";
    }
}

static void build_endpoint_url(const opcua_settings_t *cfg) {
    snprintf(s_endpoint, sizeof(s_endpoint), "opc.tcp://%u.%u.%u.%u:%u",
             cfg->ip[0], cfg->ip[1], cfg->ip[2], cfg->ip[3],
             (unsigned)WIZNET_OPCUA_TCP_PORT);
}

#if OPCUA_ENABLE
#include "open62541.h"

static UA_Server *s_server;
static bool s_server_started;
static bool s_address_space_ready;
static UA_StatusCode s_address_space_status;
static UA_UInt16 s_ns;

/* ── free-heap estimate (leak-trend for A-5 soak) ─────────────────────
 * heap_total = linker heap span; free = total - allocated (newlib mallinfo). */
extern char __StackLimit;
extern char __bss_end__;
static UA_UInt32 free_heap(void) {
    struct mallinfo m = mallinfo();
    uint32_t total = (uint32_t)(&__StackLimit - &__bss_end__);
    return (UA_UInt32)(total - (uint32_t)m.uordblks);
}

static UA_StatusCode copy_string_to_config(UA_String *dst, const char *text) {
    UA_String src = UA_STRING((char *)text);
    UA_String_clear(dst);
    return UA_String_copy(&src, dst);
}

static UA_StatusCode set_server_url(UA_ServerConfig *config, const char *url) {
    UA_Array_delete(config->serverUrls, config->serverUrlsSize,
                    &UA_TYPES[UA_TYPES_STRING]);
    config->serverUrls = NULL;
    config->serverUrlsSize = 0u;

    config->serverUrls =
        (UA_String *)UA_Array_new(1u, &UA_TYPES[UA_TYPES_STRING]);
    if(!config->serverUrls)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    UA_String ep = UA_STRING((char *)url);
    UA_StatusCode res = UA_String_copy(&ep, &config->serverUrls[0]);
    if(res != UA_STATUSCODE_GOOD) {
        UA_Array_delete(config->serverUrls, 1u, &UA_TYPES[UA_TYPES_STRING]);
        config->serverUrls = NULL;
        return res;
    }
    config->serverUrlsSize = 1u;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode configure_server_identity(UA_ServerConfig *config) {
    UA_StatusCode res = copy_string_to_config(
        &config->applicationDescription.applicationUri, OPCUA_APPLICATION_URI);
    if(res != UA_STATUSCODE_GOOD) return res;
    res = copy_string_to_config(&config->applicationDescription.productUri,
                                OPCUA_PRODUCT_URI);
    if(res != UA_STATUSCODE_GOOD) return res;

    UA_LocalizedText_clear(&config->applicationDescription.applicationName);
    config->applicationDescription.applicationName =
        UA_LOCALIZEDTEXT_ALLOC("en", "WIZnet OPC UA Node");
    config->applicationDescription.applicationType = UA_APPLICATIONTYPE_SERVER;

    res = copy_string_to_config(&config->buildInfo.manufacturerName, "WIZnet");
    if(res != UA_STATUSCODE_GOOD) return res;
    res = copy_string_to_config(&config->buildInfo.productName,
                                "WIZnet OPC UA Node (RP2350/W6300)");
    if(res != UA_STATUSCODE_GOOD) return res;
    res = copy_string_to_config(&config->buildInfo.softwareVersion, "0.2.0-A");
    if(res != UA_STATUSCODE_GOOD) return res;
    config->buildInfo.buildDate = UA_DateTime_now();

    return set_server_url(config, s_endpoint);
}

/* ── value helpers ────────────────────────────────────────────────── */
static UA_StatusCode emit_scalar(UA_DataValue *value, const void *data,
                                 const UA_DataType *type, UA_Boolean ts) {
    UA_StatusCode res = UA_Variant_setScalarCopy(&value->value, data, type);
    if(res != UA_STATUSCODE_GOOD) {
        value->hasStatus = true;
        value->status = res;
        return UA_STATUSCODE_GOOD;
    }
    value->hasValue = true;
    if(ts) {
        value->hasSourceTimestamp = true;
        value->sourceTimestamp = UA_DateTime_now();
    }
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode emit_string(UA_DataValue *value, const char *s,
                                 UA_Boolean ts) {
    UA_String str = UA_STRING((char *)s);
    return emit_scalar(value, &str, &UA_TYPES[UA_TYPES_STRING], ts);
}

static UA_StatusCode read_channel(uint16_t idx, UA_DataValue *value,
                                  UA_Boolean ts) {
    channel_value_t v;
    if(!data_table_get(idx, &v)) {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
        return UA_STATUSCODE_GOOD;
    }
    if(v.status != UA_STATUSCODE_GOOD) {
        value->hasStatus = true;
        value->status = v.status;   /* e.g. Bad_CommunicationError */
        return UA_STATUSCODE_GOOD;
    }

    switch(s_cfg->channels[idx].datatype) {
    case OPCUA_DT_FLOAT: {
        UA_Float f = (UA_Float)v.value;
        return emit_scalar(value, &f, &UA_TYPES[UA_TYPES_FLOAT], ts);
    }
    case OPCUA_DT_INT32: {
        UA_Int32 i = (UA_Int32)(v.value < 0.0 ? v.value - 0.5 : v.value + 0.5);
        return emit_scalar(value, &i, &UA_TYPES[UA_TYPES_INT32], ts);
    }
    case OPCUA_DT_BOOL: {
        UA_Boolean b = v.bool_value;
        return emit_scalar(value, &b, &UA_TYPES[UA_TYPES_BOOLEAN], ts);
    }
    case OPCUA_DT_STRING:
    default:
        return emit_string(value, v.str, ts);
    }
}

static UA_StatusCode read_diag(uint16_t kind, UA_DataValue *value,
                               UA_Boolean ts) {
    diag_counters_t d;
    data_table_diag_snapshot(&d);

    switch(kind) {
    case OPCUA_DIAG_DEVICE_NAME:
        return emit_string(value, s_cfg->device_name, ts);
    case OPCUA_DIAG_IP: {
        WiznetNetworkStatus n;
        wiznet_network_get_status(&n);
        char ip[16];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", n.ip[0], n.ip[1], n.ip[2],
                 n.ip[3]);
        return emit_string(value, ip, ts);
    }
    case OPCUA_DIAG_FW_VERSION:
        return emit_string(value, "opcua-node-0.2.0-A", ts);
    case OPCUA_DIAG_UPTIME_S: {
        UA_UInt32 up = (UA_UInt32)(to_ms_since_boot(get_absolute_time()) / 1000u);
        return emit_scalar(value, &up, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_FREE_HEAP: {
        UA_UInt32 fh = free_heap();
        return emit_scalar(value, &fh, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_REBOOT_COUNT: {
        UA_UInt32 rc = s_cfg->reboot_count;
        return emit_scalar(value, &rc, &UA_TYPES[UA_TYPES_UINT32], ts);
    }
    case OPCUA_DIAG_REBOOT_REASON: {
        const char *r = s_cfg->reboot_reason == 1 ? "watchdog"
                      : s_cfg->reboot_reason == 2 ? "user"
                                                  : "power";
        return emit_string(value, r, ts);
    }
    case OPCUA_DIAG_LINK_STATUS:
        return emit_string(value,
                           wiznet_network_is_ready() ? "up" : "down", ts);
    case OPCUA_DIAG_RAW_FRAME:
        return emit_string(value, d.last_raw, ts);
    case OPCUA_DIAG_USB_FRAMES:
        return emit_scalar(value, &d.usb_frame_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_USB_ERRORS:
        return emit_scalar(value, &d.usb_error_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_UART_FRAMES:
        return emit_scalar(value, &d.uart_frame_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_UART_ERRORS:
        return emit_scalar(value, &d.uart_error_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_MODBUS_POLLS:
        return emit_scalar(value, &d.modbus_poll_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    case OPCUA_DIAG_MODBUS_TIMEOUTS:
        return emit_scalar(value, &d.modbus_timeout_count,
                           &UA_TYPES[UA_TYPES_UINT32], ts);
    default:
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
        return UA_STATUSCODE_GOOD;
    }
}

static UA_StatusCode opcua_read_value(UA_Server *server,
                                      const UA_NodeId *session_id,
                                      void *session_context,
                                      const UA_NodeId *node_id,
                                      void *node_context,
                                      UA_Boolean include_source_timestamp,
                                      const UA_NumericRange *range,
                                      UA_DataValue *value) {
    (void)server; (void)session_id; (void)session_context; (void)node_id;

    if(range) {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }

    uintptr_t ctx = (uintptr_t)node_context;
    if(ctx >= OPCUA_DIAG_CTX_BASE)
        return read_diag((uint16_t)(ctx - OPCUA_DIAG_CTX_BASE), value,
                         include_source_timestamp);
    return read_channel((uint16_t)ctx, value, include_source_timestamp);
}

/* ── node builders ────────────────────────────────────────────────── */
static UA_StatusCode add_object(UA_UInt32 numeric_id, UA_NodeId parent,
                                UA_NodeId ref, const char *name) {
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)name);
    return UA_Server_addObjectNode(
        s_server, UA_NODEID_NUMERIC(s_ns, numeric_id), parent, ref,
        UA_QUALIFIEDNAME(s_ns, (char *)name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), attr, NULL, NULL);
}

static UA_StatusCode add_datasource_var(UA_NodeId node_id, UA_NodeId parent,
                                        const char *name, UA_UInt32 ns0_type,
                                        uintptr_t context) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)name);
    attr.dataType = UA_NODEID_NUMERIC(0, ns0_type);
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    attr.userAccessLevel = UA_ACCESSLEVELMASK_READ;

    UA_CallbackValueSource src;
    memset(&src, 0, sizeof(src));
    src.read = opcua_read_value;

    return UA_Server_addDataSourceVariableNode(
        s_server, node_id, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(s_ns, (char *)name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, src,
        (void *)context, NULL);
}

static UA_StatusCode add_string_property(UA_UInt32 numeric_id,
                                         UA_NodeId parent, const char *name,
                                         const char *text) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)name);
    attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_STRING);
    attr.valueRank = UA_VALUERANK_SCALAR;
    UA_String s = UA_STRING((char *)text);
    UA_Variant_setScalar(&attr.value, &s, &UA_TYPES[UA_TYPES_STRING]);
    return UA_Server_addVariableNode(
        s_server, UA_NODEID_NUMERIC(s_ns, numeric_id), parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(s_ns, (char *)name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attr, NULL, NULL);
}

static UA_StatusCode add_range_property(UA_UInt32 numeric_id, UA_NodeId parent,
                                        double low, double high) {
    /* EURange as a Double[2] property. Full AnalogItem/EUInformation is
     * deferred (see docs/node_map.md) to keep MINIMAL NS0 dependency-free. */
    UA_Double range[2] = {low, high};
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", "EURange");
    attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_DOUBLE);
    attr.valueRank = UA_VALUERANK_ONE_DIMENSION;
    UA_Variant_setArray(&attr.value, range, 2, &UA_TYPES[UA_TYPES_DOUBLE]);
    return UA_Server_addVariableNode(
        s_server, UA_NODEID_NUMERIC(s_ns, numeric_id), parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(s_ns, "EURange"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attr, NULL, NULL);
}

static UA_UInt32 datatype_ns0(uint8_t dt) {
    switch(dt) {
    case OPCUA_DT_INT32:  return UA_NS0ID_INT32;
    case OPCUA_DT_BOOL:   return UA_NS0ID_BOOLEAN;
    case OPCUA_DT_STRING: return UA_NS0ID_STRING;
    case OPCUA_DT_FLOAT:
    default:              return UA_NS0ID_FLOAT;
    }
}

static void add_diag_var(UA_NodeId device, OpcUaDiagKind kind,
                         const char *name, UA_UInt32 ns0_type) {
    char nid[40];
    snprintf(nid, sizeof(nid), "Diag.%s", name);
    add_datasource_var(UA_NODEID_STRING(s_ns, nid), device, name, ns0_type,
                       OPCUA_DIAG_CTX_BASE + (uintptr_t)kind);
}

static UA_StatusCode add_address_space(void) {
    if(s_ns == 0)
        s_ns = UA_Server_addNamespace(s_server, OPCUA_NODE_NS_URI);

    UA_NodeId objects = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId root    = UA_NODEID_NUMERIC(s_ns, OPCUA_ID_ROOT);
    UA_NodeId device  = UA_NODEID_NUMERIC(s_ns, OPCUA_ID_DEVICE);
    UA_NodeId chans   = UA_NODEID_NUMERIC(s_ns, OPCUA_ID_CHANNELS);

    UA_StatusCode res = add_object(OPCUA_ID_ROOT, objects,
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                   s_cfg->device_name);
    if(res != UA_STATUSCODE_GOOD) return res;

    res = add_object(OPCUA_ID_DEVICE, root,
                     UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), "Device");
    if(res != UA_STATUSCODE_GOOD) return res;

    res = add_object(OPCUA_ID_CHANNELS, root,
                     UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), "Channels");
    if(res != UA_STATUSCODE_GOOD) return res;

    /* Device diagnostics. */
    add_diag_var(device, OPCUA_DIAG_DEVICE_NAME,    "DeviceName",       UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_IP,             "IPAddress",        UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_FW_VERSION,     "FwVersion",        UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_UPTIME_S,       "Uptime_s",         UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_FREE_HEAP,      "FreeHeap",         UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_REBOOT_COUNT,   "RebootCount",      UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_REBOOT_REASON,  "RebootReason",     UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_LINK_STATUS,    "LinkStatus",       UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_RAW_FRAME,      "RawFrame",         UA_NS0ID_STRING);
    add_diag_var(device, OPCUA_DIAG_USB_FRAMES,     "UsbFrameCount",    UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_USB_ERRORS,     "UsbErrorCount",    UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_UART_FRAMES,    "UartFrameCount",   UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_UART_ERRORS,    "UartErrorCount",   UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_MODBUS_POLLS,   "ModbusPollCount",  UA_NS0ID_UINT32);
    add_diag_var(device, OPCUA_DIAG_MODBUS_TIMEOUTS,"ModbusTimeoutCount",UA_NS0ID_UINT32);

    /* Channels. */
    for(uint16_t i = 0; i < s_cfg->channel_count; i++) {
        const channel_def_t *c = &s_cfg->channels[i];
        if(!c->enabled)
            continue;

        char nid[64];
        snprintf(nid, sizeof(nid), "Channels/%s", c->name);
        UA_NodeId var_id = UA_NODEID_STRING(s_ns, nid);

        res = add_datasource_var(var_id, chans, c->name,
                                 datatype_ns0(c->datatype), (uintptr_t)i);
        if(res != UA_STATUSCODE_GOOD) {
            printf("[OPC UA] channel '%s' add failed: %s\r\n", c->name,
                   UA_StatusCode_name(res));
            continue;
        }
        if(c->unit[0] != '\0')
            add_string_property(OPCUA_ID_PROP_BASE + (UA_UInt32)i * 4u + 0u,
                                var_id, "EngineeringUnits", c->unit);
        add_range_property(OPCUA_ID_PROP_BASE + (UA_UInt32)i * 4u + 1u,
                           var_id, c->eu_low, c->eu_high);
    }
    return UA_STATUSCODE_GOOD;
}
#endif /* OPCUA_ENABLE */

int opcua_server_init(const opcua_settings_t *cfg) {
    s_cfg = cfg;
    build_endpoint_url(cfg);
    s_status.endpoint_url = s_endpoint;
    s_status.application_uri = OPCUA_APPLICATION_URI;
    s_status.channel_count = cfg->channel_count;

#if OPCUA_ENABLE
    s_address_space_ready = false;
    s_address_space_status = UA_STATUSCODE_GOOD;

    UA_ServerConfig config;
    memset(&config, 0, sizeof(config));

    /* 트랜스포트가 동시에 LISTEN 할 하드웨어 소켓 수.
     * 반드시 서버 생성 전에 지정한다. */
    UA_EventLoop_LWIP_setSocketCount(OPCUA_MAX_SOCKETS);

    UA_StatusCode res = UA_ServerConfig_setMinimalCustomBuffer(
        &config, WIZNET_OPCUA_TCP_PORT, NULL, 8192u, 8192u);

    /* 리소스 상한. open62541 기본값은 세션 100 / 보안채널 100 이라
     * 소켓 4개짜리 장치에 맞지 않는다. 트랜스포트 수와 어긋나면 TCP 는
     * 못 붙는데 서버는 여유가 있다고 판단하는 불일치가 생긴다.
     *
     * 한도 초과 시 동작이 항목마다 다르다. maxSessions 를 넘으면
     * BadTooManySessions 로 깔끔하게 거부되지만, maxSecureChannels 를
     * 넘으면 세션이 붙지 않은 채널을 먼저 강제 종료한다
     * (purgeFirstChannelWithoutSession). 즉 동작 중인 클라이언트가
     * 쫓겨나지는 않아도 핸드셰이크 중인 접속은 밀려날 수 있다. */
    if(res == UA_STATUSCODE_GOOD) {
        config.maxSecureChannels          = OPCUA_MAX_SOCKETS;
        config.maxSessions                = OPCUA_MAX_SOCKETS;
        config.maxSessionTimeout          = 10.0 * 60.0 * 1000.0; /* 10분 */
        config.maxNotificationsPerPublish = 100;   /* 한 응답이 6KB TX 버퍼에 들어가는 크기 */
        config.maxRetransmissionQueueSize = 256;   /* 구독당 상한과 동일 */
    }
    if(res == UA_STATUSCODE_GOOD)
        res = configure_server_identity(&config);

    if(res == UA_STATUSCODE_GOOD) {
        s_server = UA_Server_newWithConfig(&config);
        if(!s_server)
            res = UA_STATUSCODE_BADOUTOFMEMORY;
    }

    bool startup_done = false;
    if(res == UA_STATUSCODE_GOOD) {
        res = UA_Server_run_startup(s_server);
        startup_done = (res == UA_STATUSCODE_GOOD);
    }

    if(res != UA_STATUSCODE_GOOD) {
        if(s_server) {
            if(startup_done)
                UA_Server_run_shutdown(s_server);
            UA_Server_delete(s_server);
            s_server = NULL;
        } else {
            UA_ServerConfig_clear(&config);
        }
        s_status.state = OPCUA_SERVER_STATE_ERROR;
        s_status.network_enabled = false;
        s_status.status_text = UA_StatusCode_name(res);
        printf("[OPC UA] startup failed: %s\r\n", UA_StatusCode_name(res));
        return -1;
    }

    s_address_space_status = add_address_space();
    s_address_space_ready = (s_address_space_status == UA_STATUSCODE_GOOD);
    if(!s_address_space_ready)
        printf("[OPC UA] address space warning: %s\r\n",
               UA_StatusCode_name(s_address_space_status));

    s_server_started = true;
    s_status.state = OPCUA_SERVER_STATE_RUNNING;
    s_status.network_enabled = true;
    s_status.status_text = s_address_space_ready
                               ? "open62541 running; dynamic address space"
                               : "open62541 running; address space incomplete";
    return 0;
#else
#if OPCUA_TCP_PROBE_ENABLE
    opcua_tcp_probe_init();
    s_status.state = OPCUA_SERVER_STATE_RUNNING;
    s_status.network_enabled = true;
    s_status.status_text = "WIZnet TCP probe; open62541 not linked";
    return 0;
#else
    s_status.state = OPCUA_SERVER_STATE_READY;
    s_status.network_enabled = false;
    s_status.status_text = "OPC UA scaffold; open62541/WIZnet transport not linked";
    return 0;
#endif
#endif
}

int opcua_server_reload_nodes(const opcua_settings_t *cfg) {
    s_cfg = cfg;
    s_status.channel_count = cfg->channel_count;
#if OPCUA_ENABLE
    if(!s_server_started || !s_server)
        return -1;
    /* Delete the root subtree, then rebuild from cfg. Namespace persists. */
    UA_Server_deleteNode(s_server, UA_NODEID_NUMERIC(s_ns, OPCUA_ID_ROOT), true);
    s_address_space_status = add_address_space();
    s_address_space_ready = (s_address_space_status == UA_STATUSCODE_GOOD);
    printf("[OPC UA] node reload %s (%u channels)\r\n",
           s_address_space_ready ? "ok" : "partial",
           (unsigned)cfg->channel_count);
    return s_address_space_ready ? 0 : -1;
#else
    return 0;
#endif
}

void opcua_server_poll(void) {
#if OPCUA_ENABLE
    if(s_server_started && s_server)
        UA_Server_run_iterate(s_server, false);
#elif OPCUA_TCP_PROBE_ENABLE
    opcua_tcp_probe_poll();
#endif
}

void opcua_server_get_status(OpcUaServerStatus *status) {
    if(status)
        *status = s_status;
}

void opcua_server_print_status(void) {
    printf("[OPC UA] state=%s network=%s endpoint=%s channels=%u\r\n",
           state_name(s_status.state),
           s_status.network_enabled ? "on" : "off",
           s_status.endpoint_url ? s_status.endpoint_url : "(none)",
           (unsigned)s_status.channel_count);
    printf("[OPC UA] %s\r\n",
           s_status.status_text ? s_status.status_text : "(no status)");
#if !OPCUA_ENABLE && OPCUA_TCP_PROBE_ENABLE
    opcua_tcp_probe_print_status();
#endif
}
