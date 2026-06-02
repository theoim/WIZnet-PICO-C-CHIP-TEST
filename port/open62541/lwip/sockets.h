#ifndef OPEN62541_WIZNET_LWIP_SOCKETS_H
#define OPEN62541_WIZNET_LWIP_SOCKETS_H

/*
 * The generated open62541 amalgamation includes <lwip/sockets.h> only to get
 * AF_INET/AF_INET6 for its embedded inet_pton helper. The WIZnet port does not
 * use the lwIP socket API, so keep this shim intentionally small.
 */
#ifndef AF_INET
#define AF_INET 2
#endif

#ifndef AF_INET6
#define AF_INET6 10
#endif

#endif /* OPEN62541_WIZNET_LWIP_SOCKETS_H */
