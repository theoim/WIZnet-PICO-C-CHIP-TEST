/**
 * @file    opcua_security.h
 * @brief   Security policies and secure endpoints for the OPC UA server.
 *
 * Built only when the amalgamation was generated with the policies compiled in
 * (-DOPCUA_ENABLE_ENCRYPTION=ON). Without that the server keeps the single
 * SecurityPolicy#None endpoint it has always had, and this header's function
 * is a no-op that says so.
 *
 * What gets offered, and why both:
 *
 *   Basic256Sha256   RSA. The compatibility path. The 2026-10-15 survey found
 *                    it in the policy list of every client checked, and ECC in
 *                    none of the confirmed ones, so removing this removes the
 *                    market (see docs/crypto_cost.md section 6-5).
 *
 *   ECC_nistP256     Elliptic curve. The fast path: 259 ms of handshake
 *                    arithmetic against RSA's 1,229 ms on this part. Offered
 *                    for clients that can use it, and deprecated upstream in
 *                    favour of ECC_nistP256-AesGcm, which our vendored
 *                    open62541 does not carry yet (section 6-6).
 *
 * A policy can only use a certificate whose key type matches it, so there are
 * two certificates. Both come from tools/make_opcua_certs.sh.
 */

#ifndef OPCUA_SECURITY_H
#define OPCUA_SECURITY_H

#include "open62541.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Registers the security policies and their endpoints on @p config.
 *
 * Call after the base configuration is set up and before the server is
 * created. Failures are logged per policy and do not abort the others: a server
 * that comes up with one working policy is more useful for bring-up than one
 * that refuses to start, and the log says exactly what is missing.
 *
 * Returns UA_STATUSCODE_GOOD when at least one secure endpoint was added, or
 * when the build has no policies compiled in (nothing to do).
 */
UA_StatusCode opcua_security_apply(UA_ServerConfig *config);

/** Prints the endpoints the server will advertise. For the serial log. */
void opcua_security_report(const UA_ServerConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* OPCUA_SECURITY_H */
