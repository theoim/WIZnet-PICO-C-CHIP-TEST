/**
 * @file    opcua_security.c
 * @brief   Registers the security policies and secure endpoints.
 *
 * See opcua_security.h for which policies and why both of them.
 */

#include <stdio.h>
#include <stdint.h>

#include "opcua_security.h"

#if defined(UA_ENABLE_ENCRYPTION)

#include "opcua_certs.h"

/*
 * UA_ByteString carries a non-const pointer, and the certificates are const
 * arrays in flash. open62541's policy setup copies what it is given rather than
 * keeping the pointer, so handing it a view of read-only memory is safe - but
 * the cast is here, visible, rather than hidden behind a macro.
 */
static UA_ByteString view(const unsigned char *data, unsigned int len)
{
    UA_ByteString b;
    b.length = (size_t)len;
    b.data   = (UA_Byte *)(uintptr_t)data;
    return b;
}

static void add_policy(UA_ServerConfig *config,
                       const char      *name,
                       UA_StatusCode  (*fn)(UA_ServerConfig *,
                                            const UA_ByteString *,
                                            const UA_ByteString *),
                       const unsigned char *cert, unsigned int cert_len,
                       const unsigned char *key,  unsigned int key_len,
                       unsigned int *added)
{
    UA_ByteString c = view(cert, cert_len);
    UA_ByteString k = view(key, key_len);

    UA_StatusCode rc = fn(config, &c, &k);
    if (rc == UA_STATUSCODE_GOOD) {
        printf("[sec] policy %-16s ok   (cert %u B, key %u B)\n",
               name, cert_len, key_len);
        (*added)++;
    } else {
        /*
         * The usual cause is entropy. Both policies seed a DRBG from an
         * mbedtls_entropy_context they own, so without MBEDTLS_ENTROPY_HARDWARE_ALT
         * and a working mbedtls_hardware_poll() this fails here and nowhere else.
         */
        printf("[sec] policy %-16s FAILED 0x%08lx\n",
               name, (unsigned long)rc);
    }
}

UA_StatusCode opcua_security_apply(UA_ServerConfig *config)
{
    if (config == NULL) {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    unsigned int added = 0;

    printf("[sec] registering security policies\n");

    /* The compatibility path. Removing this removes the installed base. */
    add_policy(config, "Basic256Sha256",
               UA_ServerConfig_addSecurityPolicyBasic256Sha256,
               opcua_cert_rsa, opcua_cert_rsa_len,
               opcua_key_rsa,  opcua_key_rsa_len, &added);

    /* The fast path, for clients that offer it. */
    add_policy(config, "ECC_nistP256",
               UA_ServerConfig_addSecurityPolicyEccNistP256,
               opcua_cert_ecc, opcua_cert_ecc_len,
               opcua_key_ecc,  opcua_key_ecc_len, &added);

    if (added == 0) {
        printf("[sec] no policy initialised - server stays on None only\n");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /*
     * Two endpoints per policy: Sign and SignAndEncrypt.
     *
     * addAllEndpoints produces a mode pair for every registered policy, so the
     * list comes out at five with SecurityPolicy#None included - not three.
     * That is more than a product wants, but it is what the interoperability
     * matrix needs: a client that defaults to Sign and one that cannot do the
     * policy at all fail differently, and with only SignAndEncrypt on offer the
     * two look the same from the outside.
     *
     * SecurityPolicy#None stays as the base configuration left it. It is what
     * makes the device reachable while commissioning, and it is the control
     * that separates "server unreachable" from "no policy in common". A product
     * build drops it.
     */
    UA_StatusCode rc = UA_ServerConfig_addAllEndpoints(config);
    if (rc != UA_STATUSCODE_GOOD) {
        printf("[sec] addAllEndpoints FAILED 0x%08lx\n", (unsigned long)rc);
        return rc;
    }

    return UA_STATUSCODE_GOOD;
}

void opcua_security_report(const UA_ServerConfig *config)
{
    if (config == NULL) return;

    printf("[sec] endpoints advertised: %u\n",
           (unsigned)config->endpointsSize);

    for (size_t i = 0; i < config->endpointsSize; i++) {
        const UA_EndpointDescription *ep = &config->endpoints[i];
        const char *mode =
            (ep->securityMode == UA_MESSAGESECURITYMODE_NONE)           ? "None" :
            (ep->securityMode == UA_MESSAGESECURITYMODE_SIGN)           ? "Sign" :
            (ep->securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT) ? "SignAndEncrypt" :
                                                                          "?";

        /*
         * endpointUrl is printed because it is a likely source of a false
         * failure in the matrix: the discovery URL comes out as
         * opc.tcp://0.0.0.0:4840 from a socket bound to every address, and a
         * client that reconnects to the URL the server hands back rather than
         * the one it dialled would go nowhere. Whether open62541 rewrites it
         * per request is easier to read off a live server than out of the
         * source, so it goes in the log.
         *
         * Neither string is NUL-terminated; both are printed by length.
         */
        printf("[sec]   %.*s  %s  url=%.*s\n",
               (int)ep->securityPolicyUri.length,
               (const char *)ep->securityPolicyUri.data, mode,
               (int)ep->endpointUrl.length,
               (const char *)ep->endpointUrl.data);
    }
}

#else /* !UA_ENABLE_ENCRYPTION */

UA_StatusCode opcua_security_apply(UA_ServerConfig *config)
{
    (void)config;
    printf("[sec] build has no security policies "
           "(configure with -DOPCUA_ENABLE_ENCRYPTION=ON)\n");
    return UA_STATUSCODE_GOOD;
}

void opcua_security_report(const UA_ServerConfig *config)
{
    (void)config;
}

#endif /* UA_ENABLE_ENCRYPTION */
