#include "opcua_server.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "data_table.h"
#include "opcua_node_map.h"
#include "opcua_tcp_probe.h"
#include "pico/stdlib.h"
#include "wiznet_network.h"

#ifndef OPCUA_ENABLE
#define OPCUA_ENABLE 0
#endif

#ifndef OPCUA_TCP_PROBE_ENABLE
#define OPCUA_TCP_PROBE_ENABLE 0
#endif

#if OPCUA_ENABLE
#include "open62541.h"

typedef enum {
    OPCUA_VALUE_DEVICE_NAME = 0,
    OPCUA_VALUE_DEVICE_IP,
    OPCUA_VALUE_DEVICE_FW_VERSION,
    OPCUA_VALUE_DEVICE_UPTIME,
    OPCUA_VALUE_INPUT_RAW_FRAME,
    OPCUA_VALUE_INPUT_CH1,
    OPCUA_VALUE_INPUT_CH2,
    OPCUA_VALUE_INPUT_CH3,
    OPCUA_VALUE_INPUT_FRAME_COUNT,
    OPCUA_VALUE_INPUT_PARSE_ERROR_COUNT
} OpcUaValueKind;

static UA_Server *s_server;
static bool s_server_started;
static bool s_address_space_ready;
static UA_StatusCode s_address_space_status;
static UA_UInt16 s_namespace_index;
#endif

static OpcUaServerStatus s_status;

static const char *state_name(OpcUaServerState state) {
    switch(state) {
    case OPCUA_SERVER_STATE_DISABLED:
        return "DISABLED";
    case OPCUA_SERVER_STATE_READY:
        return "READY";
    case OPCUA_SERVER_STATE_RUNNING:
        return "RUNNING";
    case OPCUA_SERVER_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

#if OPCUA_ENABLE
static UA_StatusCode copy_string_to_config(UA_String *dst, const char *text) {
    UA_String src = UA_STRING((char *)text);
    UA_String_clear(dst);
    return UA_String_copy(&src, dst);
}

static UA_StatusCode set_server_url(UA_ServerConfig *config,
                                    const char *endpoint_url) {
    UA_Array_delete(config->serverUrls, config->serverUrlsSize,
                    &UA_TYPES[UA_TYPES_STRING]);
    config->serverUrls = NULL;
    config->serverUrlsSize = 0u;

    config->serverUrls = (UA_String *)UA_Array_new(1u,
                                                   &UA_TYPES[UA_TYPES_STRING]);
    if(!config->serverUrls)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    UA_String endpoint = UA_STRING((char *)endpoint_url);
    UA_StatusCode res = UA_String_copy(&endpoint, &config->serverUrls[0]);
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
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = copy_string_to_config(&config->applicationDescription.productUri,
                                OPCUA_PRODUCT_URI);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    UA_LocalizedText_clear(&config->applicationDescription.applicationName);
    config->applicationDescription.applicationName =
        UA_LOCALIZEDTEXT_ALLOC("en", "WIZnet OPC UA USB stdio");
    config->applicationDescription.applicationType =
        UA_APPLICATIONTYPE_SERVER;

    res = copy_string_to_config(&config->buildInfo.productUri,
                                OPCUA_PRODUCT_URI);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = copy_string_to_config(&config->buildInfo.manufacturerName, "WIZnet");
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = copy_string_to_config(&config->buildInfo.productName,
                                "WIZnet-EVB-PICO2 OPC UA prototype");
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = copy_string_to_config(&config->buildInfo.softwareVersion,
                                "0.1.0");
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = copy_string_to_config(&config->buildInfo.buildNumber,
                                "usb-stdio-open62541");
    if(res != UA_STATUSCODE_GOOD)
        return res;
    config->buildInfo.buildDate = UA_DateTime_now();

    return set_server_url(config, OPCUA_ENDPOINT_URL);
}

static UA_StatusCode read_scalar_copy(UA_DataValue *value, const void *data,
                                      const UA_DataType *type,
                                      UA_Boolean include_source_timestamp) {
    UA_StatusCode res = UA_Variant_setScalarCopy(&value->value, data, type);
    if(res != UA_STATUSCODE_GOOD) {
        value->hasStatus = true;
        value->status = res;
        return UA_STATUSCODE_GOOD;
    }

    value->hasValue = true;
    if(include_source_timestamp) {
        value->hasSourceTimestamp = true;
        value->sourceTimestamp = UA_DateTime_now();
    }

    return UA_STATUSCODE_GOOD;
}

static void format_ip(char *out, size_t out_size) {
    WiznetNetworkStatus net;
    wiznet_network_get_status(&net);
    snprintf(out, out_size, "%u.%u.%u.%u",
             net.ip[0], net.ip[1], net.ip[2], net.ip[3]);
}

static UA_StatusCode opcua_read_value(UA_Server *server,
                                      const UA_NodeId *session_id,
                                      void *session_context,
                                      const UA_NodeId *node_id,
                                      void *node_context,
                                      UA_Boolean include_source_timestamp,
                                      const UA_NumericRange *range,
                                      UA_DataValue *value) {
    (void)server;
    (void)session_id;
    (void)session_context;
    (void)node_id;

    if(range) {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }

    DataTable snapshot;
    data_table_snapshot(&snapshot);

    switch((OpcUaValueKind)(uintptr_t)node_context) {
    case OPCUA_VALUE_DEVICE_NAME: {
        UA_String text = UA_STRING("WIZnet-EVB-PICO2");
        return read_scalar_copy(value, &text, &UA_TYPES[UA_TYPES_STRING],
                                include_source_timestamp);
    }
    case OPCUA_VALUE_DEVICE_IP: {
        char ip[16];
        format_ip(ip, sizeof(ip));
        UA_String text = UA_STRING(ip);
        return read_scalar_copy(value, &text, &UA_TYPES[UA_TYPES_STRING],
                                include_source_timestamp);
    }
    case OPCUA_VALUE_DEVICE_FW_VERSION: {
        UA_String text = UA_STRING("opcua-usb-stdio-prototype");
        return read_scalar_copy(value, &text, &UA_TYPES[UA_TYPES_STRING],
                                include_source_timestamp);
    }
    case OPCUA_VALUE_DEVICE_UPTIME: {
        UA_UInt32 uptime =
            (UA_UInt32)(to_ms_since_boot(get_absolute_time()) / 1000u);
        return read_scalar_copy(value, &uptime, &UA_TYPES[UA_TYPES_UINT32],
                                include_source_timestamp);
    }
    case OPCUA_VALUE_INPUT_RAW_FRAME: {
        UA_String text = UA_STRING(snapshot.raw);
        return read_scalar_copy(value, &text, &UA_TYPES[UA_TYPES_STRING],
                                include_source_timestamp);
    }
    case OPCUA_VALUE_INPUT_CH1: {
        UA_Float ch = snapshot.ch1;
        return read_scalar_copy(value, &ch, &UA_TYPES[UA_TYPES_FLOAT],
                                include_source_timestamp);
    }
    case OPCUA_VALUE_INPUT_CH2: {
        UA_Float ch = snapshot.ch2;
        return read_scalar_copy(value, &ch, &UA_TYPES[UA_TYPES_FLOAT],
                                include_source_timestamp);
    }
    case OPCUA_VALUE_INPUT_CH3: {
        UA_Float ch = snapshot.ch3;
        return read_scalar_copy(value, &ch, &UA_TYPES[UA_TYPES_FLOAT],
                                include_source_timestamp);
    }
    case OPCUA_VALUE_INPUT_FRAME_COUNT: {
        UA_UInt32 count = snapshot.frame_count;
        return read_scalar_copy(value, &count, &UA_TYPES[UA_TYPES_UINT32],
                                include_source_timestamp);
    }
    case OPCUA_VALUE_INPUT_PARSE_ERROR_COUNT: {
        UA_UInt32 count = snapshot.parse_error_count;
        return read_scalar_copy(value, &count, &UA_TYPES[UA_TYPES_UINT32],
                                include_source_timestamp);
    }
    default:
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
        return UA_STATUSCODE_GOOD;
    }
}

static UA_StatusCode add_object_node(UA_Server *server, UA_UInt16 ns,
                                     UA_UInt32 numeric_id,
                                     UA_NodeId parent_id,
                                     UA_NodeId reference_type,
                                     const char *browse_name,
                                     const char *display_name) {
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)display_name);

    return UA_Server_addObjectNode(
        server, UA_NODEID_NUMERIC(ns, numeric_id), parent_id, reference_type,
        UA_QUALIFIEDNAME(ns, (char *)browse_name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), attr, NULL, NULL);
}

static UA_StatusCode add_data_variable(UA_Server *server, UA_UInt16 ns,
                                       UA_NodeId parent_id,
                                       const char *node_string_id,
                                       const char *browse_name,
                                       const char *display_name,
                                       UA_NodeId data_type,
                                       OpcUaValueKind kind) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT("en", (char *)display_name);
    attr.dataType = data_type;
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    attr.userAccessLevel = UA_ACCESSLEVELMASK_READ;

    UA_CallbackValueSource source;
    memset(&source, 0, sizeof(source));
    source.read = opcua_read_value;

    return UA_Server_addDataSourceVariableNode(
        server, UA_NODEID_STRING(ns, (char *)node_string_id), parent_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(ns, (char *)browse_name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, source,
        (void *)(uintptr_t)kind, NULL);
}

static UA_StatusCode add_address_space(UA_Server *server) {
    s_namespace_index = UA_Server_addNamespace(server, OPCUA_APPLICATION_URI);

    UA_NodeId root_id =
        UA_NODEID_NUMERIC(s_namespace_index, OPCUA_NODE_ROOT_NUMERIC_ID);
    UA_NodeId device_id =
        UA_NODEID_NUMERIC(s_namespace_index, OPCUA_NODE_DEVICE_NUMERIC_ID);
    UA_NodeId input_id =
        UA_NODEID_NUMERIC(s_namespace_index, OPCUA_NODE_INPUT_NUMERIC_ID);

    UA_StatusCode res = add_object_node(
        server, s_namespace_index, OPCUA_NODE_ROOT_NUMERIC_ID,
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        "Sensor_Node", "Sensor_Node");
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_object_node(server, s_namespace_index,
                          OPCUA_NODE_DEVICE_NUMERIC_ID, root_id,
                          UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                          "Device", "Device");
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_object_node(server, s_namespace_index,
                          OPCUA_NODE_INPUT_NUMERIC_ID, root_id,
                          UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                          "USB_Stdio", "USB_Stdio");
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_data_variable(
        server, s_namespace_index, device_id, OPCUA_NODE_DEVICE_NAME,
        "DeviceName", "DeviceName", UA_NODEID_NUMERIC(0, UA_NS0ID_STRING),
        OPCUA_VALUE_DEVICE_NAME);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_data_variable(
        server, s_namespace_index, device_id, OPCUA_NODE_DEVICE_IP,
        "IPAddress", "IPAddress", UA_NODEID_NUMERIC(0, UA_NS0ID_STRING),
        OPCUA_VALUE_DEVICE_IP);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_data_variable(
        server, s_namespace_index, device_id, OPCUA_NODE_DEVICE_FW_VERSION,
        "FwVersion", "FwVersion", UA_NODEID_NUMERIC(0, UA_NS0ID_STRING),
        OPCUA_VALUE_DEVICE_FW_VERSION);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_data_variable(
        server, s_namespace_index, device_id, OPCUA_NODE_DEVICE_UPTIME,
        "Uptime_s", "Uptime_s", UA_NODEID_NUMERIC(0, UA_NS0ID_UINT32),
        OPCUA_VALUE_DEVICE_UPTIME);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_data_variable(
        server, s_namespace_index, input_id, OPCUA_NODE_INPUT_RAW_FRAME,
        "RawFrame", "RawFrame", UA_NODEID_NUMERIC(0, UA_NS0ID_STRING),
        OPCUA_VALUE_INPUT_RAW_FRAME);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_data_variable(
        server, s_namespace_index, input_id, OPCUA_NODE_INPUT_CH1,
        "Channel_1", "Channel_1", UA_NODEID_NUMERIC(0, UA_NS0ID_FLOAT),
        OPCUA_VALUE_INPUT_CH1);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_data_variable(
        server, s_namespace_index, input_id, OPCUA_NODE_INPUT_CH2,
        "Channel_2", "Channel_2", UA_NODEID_NUMERIC(0, UA_NS0ID_FLOAT),
        OPCUA_VALUE_INPUT_CH2);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_data_variable(
        server, s_namespace_index, input_id, OPCUA_NODE_INPUT_CH3,
        "Channel_3", "Channel_3", UA_NODEID_NUMERIC(0, UA_NS0ID_FLOAT),
        OPCUA_VALUE_INPUT_CH3);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = add_data_variable(
        server, s_namespace_index, input_id, OPCUA_NODE_INPUT_FRAME_COUNT,
        "FrameCount", "FrameCount", UA_NODEID_NUMERIC(0, UA_NS0ID_UINT32),
        OPCUA_VALUE_INPUT_FRAME_COUNT);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    return add_data_variable(
        server, s_namespace_index, input_id,
        OPCUA_NODE_INPUT_PARSE_ERROR_COUNT, "ParseErrorCount",
        "ParseErrorCount", UA_NODEID_NUMERIC(0, UA_NS0ID_UINT32),
        OPCUA_VALUE_INPUT_PARSE_ERROR_COUNT);
}
#endif

int opcua_server_init(void) {
    s_status.endpoint_url = OPCUA_ENDPOINT_URL;
    s_status.application_uri = OPCUA_APPLICATION_URI;

#if OPCUA_ENABLE
    s_address_space_ready = false;
    s_address_space_status = UA_STATUSCODE_GOOD;

    UA_ServerConfig config;
    memset(&config, 0, sizeof(config));

    UA_StatusCode res = UA_ServerConfig_setMinimalCustomBuffer(
        &config, WIZNET_OPCUA_TCP_PORT, NULL, 8192u, 8192u);
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

    s_address_space_status = add_address_space(s_server);
    s_address_space_ready =
        (s_address_space_status == UA_STATUSCODE_GOOD);
    if(!s_address_space_ready) {
        printf("[OPC UA] address space warning: %s\r\n",
               UA_StatusCode_name(s_address_space_status));
        printf("[OPC UA] server continues so UAExpert can test TCP/session\r\n");
    }

    s_server_started = true;
    s_status.state = OPCUA_SERVER_STATE_RUNNING;
    s_status.network_enabled = true;
    s_status.status_text = s_address_space_ready
                               ? "open62541 server running on WIZnet TCP"
                               : "open62541 running; custom nodes unavailable";
    return 0;
#else
#if OPCUA_TCP_PROBE_ENABLE
    opcua_tcp_probe_init();
    s_status.state = OPCUA_SERVER_STATE_RUNNING;
    s_status.network_enabled = true;
    s_status.status_text =
        "WIZnet TCP probe enabled; HEL/ACK only, open62541 services are not linked yet";
    return 0;
#else
    s_status.state = OPCUA_SERVER_STATE_READY;
    s_status.network_enabled = false;
    s_status.status_text =
        "OPC UA scaffold ready; open62541/WIZnet transport is not linked in this build";
    return 0;
#endif
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
    printf("[OPC UA] state=%s network=%s endpoint=%s\r\n",
           state_name(s_status.state),
           s_status.network_enabled ? "on" : "off",
           s_status.endpoint_url ? s_status.endpoint_url : "(none)");
    printf("[OPC UA] application=%s\r\n",
           s_status.application_uri ? s_status.application_uri : "(none)");
    printf("[OPC UA] %s\r\n",
           s_status.status_text ? s_status.status_text : "(no status)");
#if !OPCUA_ENABLE && OPCUA_TCP_PROBE_ENABLE
    opcua_tcp_probe_print_status();
#elif OPCUA_ENABLE
    if(s_server_started) {
        if(s_address_space_ready) {
            printf("[OPC UA] namespace=%u nodes=Sensor_Node/Device/USB_Stdio\r\n",
                   (unsigned int)s_namespace_index);
        } else {
            printf("[OPC UA] namespace unavailable: %s\r\n",
                   UA_StatusCode_name(s_address_space_status));
        }
    }
#endif
}
