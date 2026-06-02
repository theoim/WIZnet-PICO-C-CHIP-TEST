#ifndef OPCUA_TCP_PROBE_H
#define OPCUA_TCP_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OPCUA_TCP_PROBE_STATE_DISABLED = 0,
    OPCUA_TCP_PROBE_STATE_WAIT_LINK,
    OPCUA_TCP_PROBE_STATE_SOCKET_OPEN,
    OPCUA_TCP_PROBE_STATE_LISTENING,
    OPCUA_TCP_PROBE_STATE_CONNECTED,
    OPCUA_TCP_PROBE_STATE_ERROR
} OpcUaTcpProbeState;

typedef struct {
    OpcUaTcpProbeState state;
    bool initialized;
    uint8_t socket_id;
    uint16_t port;
    uint8_t peer_ip[4];
    uint16_t peer_port;
    uint32_t connection_count;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t hello_count;
    uint32_t open_secure_channel_count;
    uint32_t error_count;
    char last_message_type[5];
    char last_endpoint_url[96];
    const char *status_text;
} OpcUaTcpProbeStatus;

int opcua_tcp_probe_init(void);
void opcua_tcp_probe_poll(void);
void opcua_tcp_probe_get_status(OpcUaTcpProbeStatus *status);
void opcua_tcp_probe_print_status(void);

#endif /* OPCUA_TCP_PROBE_H */
