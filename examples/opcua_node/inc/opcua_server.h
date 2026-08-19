#ifndef OPCUA_SERVER_H
#define OPCUA_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "opcua_settings.h"

/* Application identity (static). The endpoint URL is built at runtime from the
 * configured IP; see opcua_server_init(). */
#define OPCUA_APPLICATION_URI "urn:WIZnet:opcua-node"
#define OPCUA_PRODUCT_URI     "urn:WIZnet:RP2350"

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
    uint16_t channel_count;
} OpcUaServerStatus;

/* Start the server and build the dynamic address space from cfg. cfg must
 * remain valid for the server lifetime (values are read live). */
int  opcua_server_init(const opcua_settings_t *cfg);
void opcua_server_poll(void);
void opcua_server_get_status(OpcUaServerStatus *status);
void opcua_server_print_status(void);

/* Hot reload (A-4): tear down the channel/device subtree and rebuild it from
 * cfg without restarting the server or dropping the TCP endpoint. Returns 0 on
 * success. Network/endpoint changes still require a reboot. */
int  opcua_server_reload_nodes(const opcua_settings_t *cfg);

#endif /* OPCUA_SERVER_H */
