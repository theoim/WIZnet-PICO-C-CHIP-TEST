#include "opcua_tcp_probe.h"

#include <stdio.h>
#include <string.h>

#include "socket.h"
#include "wiznet_network.h"
#include "wizchip_conf.h"

#define OPCUA_TCP_PROBE_SOCKET 0u
#define OPCUA_TCP_PROBE_RX_MAX 512u
#define OPCUA_TCP_ACK_BUFFER_SIZE 8192u
#define OPCUA_STATUS_BAD_NOT_IMPLEMENTED 0x80010000u

static OpcUaTcpProbeStatus s_status;
static uint8_t s_rx_buf[OPCUA_TCP_PROBE_RX_MAX];

static const char *state_name(OpcUaTcpProbeState state) {
    switch(state) {
    case OPCUA_TCP_PROBE_STATE_DISABLED:
        return "DISABLED";
    case OPCUA_TCP_PROBE_STATE_WAIT_LINK:
        return "WAIT_LINK";
    case OPCUA_TCP_PROBE_STATE_SOCKET_OPEN:
        return "SOCKET_OPEN";
    case OPCUA_TCP_PROBE_STATE_LISTENING:
        return "LISTENING";
    case OPCUA_TCP_PROBE_STATE_CONNECTED:
        return "CONNECTED";
    case OPCUA_TCP_PROBE_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static uint32_t read_u32le(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_u32le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
    p[2] = (uint8_t)((value >> 16) & 0xffu);
    p[3] = (uint8_t)((value >> 24) & 0xffu);
}

static int send_all(const uint8_t *buf, uint16_t len) {
    uint16_t sent_total = 0u;

    while(sent_total < len) {
        int32_t sent = send(OPCUA_TCP_PROBE_SOCKET,
                            (uint8_t *)buf + sent_total,
                            (uint16_t)(len - sent_total));
        if(sent <= 0) {
            s_status.error_count++;
            s_status.status_text = "send failed";
            return -1;
        }

        sent_total = (uint16_t)(sent_total + (uint16_t)sent);
    }

    s_status.tx_count++;
    return 0;
}

static void parse_hello_endpoint(const uint8_t *buf, uint16_t len) {
    s_status.last_endpoint_url[0] = '\0';

    if(len < 32u)
        return;

    int32_t endpoint_len = (int32_t)read_u32le(buf + 28u);
    if(endpoint_len <= 0)
        return;

    if((uint32_t)endpoint_len > (uint32_t)(len - 32u))
        return;

    size_t copy_len = (size_t)endpoint_len;
    if(copy_len >= sizeof(s_status.last_endpoint_url))
        copy_len = sizeof(s_status.last_endpoint_url) - 1u;

    memcpy(s_status.last_endpoint_url, buf + 32u, copy_len);
    s_status.last_endpoint_url[copy_len] = '\0';
}

static int send_ack(void) {
    uint8_t ack[28] = {
        'A', 'C', 'K', 'F',
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };

    write_u32le(ack + 4u, sizeof(ack));
    write_u32le(ack + 8u, 0u);
    write_u32le(ack + 12u, OPCUA_TCP_ACK_BUFFER_SIZE);
    write_u32le(ack + 16u, OPCUA_TCP_ACK_BUFFER_SIZE);
    write_u32le(ack + 20u, 0u);
    write_u32le(ack + 24u, 0u);

    if(send_all(ack, sizeof(ack)) != 0)
        return -1;

    printf("[OPC UA TCP] tx ACKF receiveBuf=%u sendBuf=%u\r\n",
           (unsigned)OPCUA_TCP_ACK_BUFFER_SIZE,
           (unsigned)OPCUA_TCP_ACK_BUFFER_SIZE);
    return 0;
}

static int send_error_and_disconnect(const char *reason) {
    uint8_t err[128];
    size_t reason_len = strlen(reason);
    if(reason_len > sizeof(err) - 16u)
        reason_len = sizeof(err) - 16u;

    uint32_t message_size = (uint32_t)(16u + reason_len);
    err[0] = 'E';
    err[1] = 'R';
    err[2] = 'R';
    err[3] = 'F';
    write_u32le(err + 4u, message_size);
    write_u32le(err + 8u, OPCUA_STATUS_BAD_NOT_IMPLEMENTED);
    write_u32le(err + 12u, (uint32_t)reason_len);
    memcpy(err + 16u, reason, reason_len);

    if(send_all(err, (uint16_t)message_size) != 0)
        return -1;

    printf("[OPC UA TCP] tx ERRF status=BadNotImplemented reason=\"%s\"\r\n",
           reason);
    disconnect(OPCUA_TCP_PROBE_SOCKET);
    return 0;
}

static void process_rx(const uint8_t *buf, uint16_t len) {
    if(len < 8u) {
        s_status.error_count++;
        s_status.status_text = "received short OPC UA TCP header";
        printf("[OPC UA TCP] rx short frame bytes=%u\r\n", len);
        return;
    }

    s_status.last_message_type[0] = (char)buf[0];
    s_status.last_message_type[1] = (char)buf[1];
    s_status.last_message_type[2] = (char)buf[2];
    s_status.last_message_type[3] = (char)buf[3];
    s_status.last_message_type[4] = '\0';

    uint32_t declared_size = read_u32le(buf + 4u);
    printf("[OPC UA TCP] rx %s bytes=%u declared=%lu\r\n",
           s_status.last_message_type,
           len,
           (unsigned long)declared_size);

    if(memcmp(buf, "HELF", 4u) == 0) {
        s_status.hello_count++;
        parse_hello_endpoint(buf, len);
        if(s_status.last_endpoint_url[0]) {
            printf("[OPC UA TCP] hello endpoint=\"%s\"\r\n",
                   s_status.last_endpoint_url);
        }

        if(send_ack() == 0)
            s_status.status_text = "HEL received and ACK sent; waiting for OPN";
        return;
    }

    if(memcmp(buf, "OPNF", 4u) == 0) {
        s_status.open_secure_channel_count++;
        s_status.status_text =
            "OpenSecureChannel received; open62541 services are not linked yet";
        send_error_and_disconnect("open62541 services are not linked in this build");
        return;
    }

    s_status.error_count++;
    s_status.status_text = "unsupported OPC UA TCP message type";
    send_error_and_disconnect("unsupported OPC UA TCP message in probe build");
}

static void close_probe_socket(void) {
    uint8_t sr = getSn_SR(OPCUA_TCP_PROBE_SOCKET);
    if(sr != SOCK_CLOSED)
        close(OPCUA_TCP_PROBE_SOCKET);
}

static void poll_socket(void) {
    int32_t ret;
    uint16_t size;
    uint8_t dest_ip[4];
    uint16_t dest_port;

    switch(getSn_SR(OPCUA_TCP_PROBE_SOCKET)) {
    case SOCK_CLOSED:
        ret = socket(OPCUA_TCP_PROBE_SOCKET,
                     Sn_MR_TCP,
                     WIZNET_OPCUA_TCP_PORT,
                     SF_TCP_NODELAY);
        if(ret != OPCUA_TCP_PROBE_SOCKET) {
            s_status.state = OPCUA_TCP_PROBE_STATE_ERROR;
            s_status.error_count++;
            s_status.status_text = "socket open failed";
            printf("[OPC UA TCP] socket open failed ret=%ld\r\n",
                   (long)ret);
            return;
        }

        s_status.state = OPCUA_TCP_PROBE_STATE_SOCKET_OPEN;
        s_status.status_text = "socket opened";
        break;

    case SOCK_INIT:
        ret = listen(OPCUA_TCP_PROBE_SOCKET);
        if(ret != SOCK_OK) {
            s_status.state = OPCUA_TCP_PROBE_STATE_ERROR;
            s_status.error_count++;
            s_status.status_text = "listen failed";
            printf("[OPC UA TCP] listen failed ret=%ld\r\n", (long)ret);
            return;
        }

        s_status.state = OPCUA_TCP_PROBE_STATE_LISTENING;
        s_status.status_text = "listening for UAExpert TCP connection";
        printf("[OPC UA TCP] listening on port %u\r\n",
               (unsigned)WIZNET_OPCUA_TCP_PORT);
        break;

    case SOCK_LISTEN:
        s_status.state = OPCUA_TCP_PROBE_STATE_LISTENING;
        break;

    case SOCK_ESTABLISHED:
        s_status.state = OPCUA_TCP_PROBE_STATE_CONNECTED;
        if(getSn_IR(OPCUA_TCP_PROBE_SOCKET) & Sn_IR_CON) {
            getSn_DIPR(OPCUA_TCP_PROBE_SOCKET, dest_ip);
            dest_port = getSn_DPORT(OPCUA_TCP_PROBE_SOCKET);
            memcpy(s_status.peer_ip, dest_ip, sizeof(s_status.peer_ip));
            s_status.peer_port = dest_port;
            s_status.connection_count++;
            setSn_IR(OPCUA_TCP_PROBE_SOCKET, Sn_IR_CON);
            s_status.status_text = "client connected";
            printf("[OPC UA TCP] client connected %u.%u.%u.%u:%u\r\n",
                   dest_ip[0], dest_ip[1], dest_ip[2], dest_ip[3],
                   dest_port);
        }

        size = getSn_RX_RSR(OPCUA_TCP_PROBE_SOCKET);
        if(size > 0u) {
            if(size > sizeof(s_rx_buf))
                size = sizeof(s_rx_buf);

            ret = recv(OPCUA_TCP_PROBE_SOCKET, s_rx_buf, size);
            if(ret > 0) {
                s_status.rx_count++;
                process_rx(s_rx_buf, (uint16_t)ret);
            } else if(ret < 0) {
                s_status.error_count++;
                s_status.status_text = "recv failed";
                printf("[OPC UA TCP] recv failed ret=%ld\r\n", (long)ret);
            }
        }
        break;

    case SOCK_CLOSE_WAIT:
        printf("[OPC UA TCP] close wait, disconnecting\r\n");
        disconnect(OPCUA_TCP_PROBE_SOCKET);
        s_status.status_text = "client disconnected";
        break;

    default:
        break;
    }
}

int opcua_tcp_probe_init(void) {
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.socket_id = OPCUA_TCP_PROBE_SOCKET;
    s_status.port = WIZNET_OPCUA_TCP_PORT;
    s_status.state = OPCUA_TCP_PROBE_STATE_WAIT_LINK;
    s_status.status_text = "waiting for WIZnet PHY link";
    s_status.last_message_type[0] = '-';
    s_status.last_message_type[1] = '\0';
    close_probe_socket();

    printf("[OPC UA TCP] probe enabled on socket %u port %u\r\n",
           (unsigned)s_status.socket_id,
           (unsigned)s_status.port);
    return 0;
}

void opcua_tcp_probe_poll(void) {
    if(!s_status.initialized)
        return;

    if(!wiznet_network_is_ready()) {
        close_probe_socket();
        s_status.state = OPCUA_TCP_PROBE_STATE_WAIT_LINK;
        s_status.status_text = "waiting for WIZnet PHY link";
        return;
    }

    poll_socket();
}

void opcua_tcp_probe_get_status(OpcUaTcpProbeStatus *status) {
    if(status)
        *status = s_status;
}

void opcua_tcp_probe_print_status(void) {
    printf("[OPC UA TCP] state=%s socket=%u port=%u peer=%u.%u.%u.%u:%u\r\n",
           state_name(s_status.state),
           (unsigned)s_status.socket_id,
           (unsigned)s_status.port,
           s_status.peer_ip[0], s_status.peer_ip[1],
           s_status.peer_ip[2], s_status.peer_ip[3],
           s_status.peer_port);
    printf("[OPC UA TCP] conn=%lu rx=%lu tx=%lu hel=%lu opn=%lu err=%lu last=%s\r\n",
           (unsigned long)s_status.connection_count,
           (unsigned long)s_status.rx_count,
           (unsigned long)s_status.tx_count,
           (unsigned long)s_status.hello_count,
           (unsigned long)s_status.open_secure_channel_count,
           (unsigned long)s_status.error_count,
           s_status.last_message_type);
    if(s_status.last_endpoint_url[0]) {
        printf("[OPC UA TCP] last endpoint=\"%s\"\r\n",
               s_status.last_endpoint_url);
    }
    printf("[OPC UA TCP] %s\r\n",
           s_status.status_text ? s_status.status_text : "(no status)");
}
