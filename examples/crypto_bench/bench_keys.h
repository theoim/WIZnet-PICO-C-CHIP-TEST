/**
 * @file    bench_keys.h
 * @brief   See bench_keys.c. Test material only - never use these anywhere.
 */

#ifndef __BENCH_KEYS_H__
#define __BENCH_KEYS_H__

#include <stddef.h>

/** PKCS#1 DER of a 2048-bit private key. */
const unsigned char *bench_key_der(size_t *len);

/** DER of a self-signed certificate holding the matching public key. */
const unsigned char *bench_cert_der(size_t *len);

#endif /* __BENCH_KEYS_H__ */
