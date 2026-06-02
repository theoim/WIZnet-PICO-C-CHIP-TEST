#ifndef OPCUA_SERVER_H
#define OPCUA_SERVER_H

#include <stdbool.h>

#define OPCUA_ENDPOINT_URL "opc.tcp://192.168.11.2:4840"
#define OPCUA_APPLICATION_URI "urn:WIZnet:EVB-PICO2:OpcUaUsbStdio"
#define OPCUA_PRODUCT_URI "urn:WIZnet:RP2350"

typedef enum {
    OPCUA_SERVER_STATE_DISABLED = 0,
    OPCUA_SERVER_STATE_READY,
    OPCUA_SERVER_STATE_RUNNING,
    OPCUA_SERVER_STATE_ERROR
} OpcUaServerState;

typedef struct {
    OpcUaServerState state;
    bool network_enabled;
    const char *endpoint_url;
    const char *application_uri;
    const char *status_text;
} OpcUaServerStatus;

int opcua_server_init(void);
void opcua_server_poll(void);
void opcua_server_get_status(OpcUaServerStatus *status);
void opcua_server_print_status(void);

#endif /* OPCUA_SERVER_H */
