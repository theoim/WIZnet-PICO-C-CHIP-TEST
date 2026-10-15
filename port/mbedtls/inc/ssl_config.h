/**
    \file ssl_config.h

    \brief Configuration options (set of defines)

    This set of compile-time options may be used to enable
    or disable features selectively, and reduce the global
    memory footprint.
*/
/*
    Copyright (C) 2006-2018, ARM Limited, All Rights Reserved
    SPDX-License-Identifier: Apache-2.0

    Licensed under the Apache License, Version 2.0 (the "License"); you may
    not use this file except in compliance with the License.
    You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
    WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

    This file is part of mbed TLS (https://tls.mbed.org)
*/

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#include "mbedtls/check_config.h"

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_DEPRECATE)
#define _CRT_SECURE_NO_DEPRECATE 1
#endif

/*
 * Bignum multiply in assembly rather than C.
 *
 * mbedTLS ships a Thumb-1 path for gcc (library/bn_mul.h), which is what a
 * Cortex-M0+ gets. The part has no 32x32 multiply instruction, so every limb
 * product is synthesised either way - but the hand-written version does it with
 * far fewer instructions than the compiler emits.
 *
 * This was commented out, and with RSA-2048 private operations measured at
 * 2.27 s on an RP2040 it is the first thing to try: it costs nothing but a
 * rebuild. Measure before and after rather than assuming - see
 * examples/crypto_bench.
 *
 * -DBENCH_NO_ASM=1 at configure time takes it back out, so an A/B pair of
 * images differs in this one thing and nothing else.
 */
#if !defined(BENCH_NO_ASM)
#define MBEDTLS_HAVE_ASM
#endif
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_REMOVE_ARC4_CIPHERSUITES
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#define MBEDTLS_PK_PARSE_EC_EXTENDED
#define MBEDTLS_ERROR_STRERROR_DUMMY
#define MBEDTLS_GENPRIME

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_PK_RSA_ALT_SUPPORT
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

#define MBEDTLS_SSL_ALL_ALERT_MESSAGES
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_AESNI_C
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_DHM_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_OID_C

#define MBEDTLS_PEM_PARSE_C

#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_SHA256_C

#define MBEDTLS_ECP_DP_SECP256R1_ENABLED     /* P-256: ECDHE-RSA 핸드셰이크에 필요 */

/*
 * Faster modular reduction for the NIST curves.
 *
 * This file replaces mbedtls_config.h entirely (MBEDTLS_CONFIG_FILE), and the
 * upstream default enables this option - so leaving it out silently turned it
 * off. That matters because reduction is where P-256 spends its time: the first
 * ECC measurements (2026-10-15, RP2040 2,458 ms / RP2350 1,697 ms per handshake)
 * were taken without it, while RSA was measured with its assembly path on. The
 * two were not compared on equal terms.
 *
 * It costs code size and no RAM. -DBENCH_NO_ECP_OPTIM=1 takes it back out so
 * the pair of images differs in this one thing - see examples/crypto_bench.
 */
#if !defined(BENCH_NO_ECP_OPTIM)
#define MBEDTLS_ECP_NIST_OPTIM
#endif
#define MBEDTLS_ECDH_C                          /* ECDH 연산 (ECDHE 키 교환) */
#define MBEDTLS_ECDSA_C                         /* ECDSA 서명 검증 (EC 인증서 체인) */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED  /* TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED /* TLS_ECDHE_ECDSA_*: ECDSA 인증서 서버 (Cloudflare 등) */
#define MBEDTLS_ENTROPY_C                       /* 엔트로피 수집기 (CTR-DRBG 의존) */
#define MBEDTLS_SHA1_C                          /* 일부 CA 체인 서명 검증에 필요 */
#define MBEDTLS_SHA512_C                        /* SHA-512 */
#define MBEDTLS_SHA384_C                        /* SHA-384: GTS Root R4 (ecdsa-with-SHA384) 파싱 필요 (mbedTLS 3.x 분리됨) */

#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_VERSION_C

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C
#define MBEDTLS_X509_CSR_PARSE_C

#define MBEDTLS_XTEA_C

#define MBEDTLS_MPI_MAX_SIZE 1024      /**< Maximum number of bytes for usable MPIs. */
#define MBEDTLS_ENTROPY_MAX_SOURCES 10 /**< Maximum number of sources supported */
#if defined(MBEDTLS_USER_CONFIG_FILE)
#include MBEDTLS_USER_CONFIG_FILE
#endif

#endif /* MBEDTLS_CONFIG_H */