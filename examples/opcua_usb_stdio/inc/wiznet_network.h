#ifndef WIZNET_NETWORK_H
#define WIZNET_NETWORK_H

#include <stdbool.h>
#include <stdint.h>

#define WIZNET_OPCUA_TCP_PORT 4840u

typedef enum {
    WIZNET_NETWORK_STATE_DISABLED = 0,
    WIZNET_NETWORK_STATE_INIT,
    WIZNET_NETWORK_STATE_LINK_DOWN,
    WIZNET_NETWORK_STATE_LINK_UP,
    WIZNET_NETWORK_STATE_ERROR
} WiznetNetworkState;

typedef struct {
    WiznetNetworkState state;
    bool initialized;
    bool link_up;
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t subnet[4];
    uint8_t gateway[4];
    const char *status_text;
} WiznetNetworkStatus;

int wiznet_network_init(void);
void wiznet_network_poll(void);
void wiznet_network_get_status(WiznetNetworkStatus *status);
void wiznet_network_print_status(void);
bool wiznet_network_is_ready(void);

#endif /* WIZNET_NETWORK_H */
