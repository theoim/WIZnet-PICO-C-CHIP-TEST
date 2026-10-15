/**
 * @file    crypto_bench.c
 * @brief   What RSA costs on this board, measured rather than estimated.
 *
 * OPC UA over opc.tcp does not use TLS. Its security lives in
 * UA-SecureConversation (Part 6): certificates are exchanged and a symmetric key
 * is derived in OpenSecureChannel, and every message after that is signed and
 * encrypted with cheap symmetric primitives. So the cost is not spread evenly -
 * it is concentrated in one exchange, and that exchange contains two RSA private
 * key operations.
 *
 * That matters more here than the total would suggest, because the server is a
 * single-core cooperative loop. While an RSA private operation runs, nothing
 * else does: no socket polling, no watchdog kick, no USB CDC. A client that is
 * waiting on OpenSecureChannel will give up if the answer takes longer than its
 * timeout, and every other connected session stops being served for the same
 * duration.
 *
 * The question this answers is therefore not "is RSA supported" but "how long
 * does the board stop", on a Cortex-M0+ with no hardware SHA, no fast multiplier
 * and no crypto accelerator.
 *
 * ---------------------------------------------------------------------------
 *
 * What is timed, and why each one is here:
 *
 *   sign (private)       the server signing its half of OpenSecureChannel
 *   OAEP decrypt (priv)  the server opening the client's nonce
 *   verify (public)      checking the client's signature - public exponent is
 *                        65537, so this should be two orders cheaper
 *   OAEP encrypt (pub)   sealing the nonce back to the client
 *   parse key            once at startup, from provisioned DER
 *   parse certificate    once per connecting client
 *   SHA-256 / AES-256    the per-message cost, over a realistic 8 KB buffer
 *   HMAC-SHA256          one block of P-SHA256 key derivation
 *
 * The two private operations added together are NOT the answer on their own, and
 * the first version of this file was wrong to add them up that way. Three things
 * were missing, all of them found by reading the policy implementation rather
 * than the specification:
 *
 *   OAEP works in blocks. plainTextBlockSize is 256 - 42 = 214 bytes
 *   (securitypolicy_basic256sha256.c:36,131) and the encrypted part of an OPN
 *   carries a 256-byte signature on its own, so it never fits in one. The policy
 *   loops a block at a time (securitypolicy_common.c, mbedtls_encrypt_rsaOaep),
 *   so a normal OPN is two decrypts, not one.
 *
 *   The response is encrypted too, with the client's public key. Cheap per
 *   block, but there are two of those as well.
 *
 *   A certificate is verified, not merely parsed. Parsing is a few hundred
 *   microseconds; verifying its signature is another public-key operation.
 *
 * Counting one decrypt and no response encryption understated the real cost by
 * about 1.6x, which on an RP2040 is the difference between clearing a client's
 * five-second timeout and not.
 *
 * ---------------------------------------------------------------------------
 *
 * The RNG here is a counter, not entropy. It exists because mbedTLS blinds RSA
 * private operations and needs a source to do it; blinding is kept because
 * removing it would measure something this firmware must not ship. A real build
 * has to seed CTR-DRBG from the ring oscillator (RP2040 has no TRNG - that is
 * RP2350 only), and that is a correctness requirement, not a timing one.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "mbedtls/rsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"

#include "bench_keys.h"
#include "wiz_entropy.h"

#define THROUGHPUT_BYTES    8192        /* the server's send buffer size */

/*
 * Which image is actually running.
 *
 * This repository has several build directories that each produce a
 * crypto_bench.uf2, and a number attributed to the wrong one is worse than no
 * number at all. So the board states its own identity before it prints a single
 * measurement, and the clock is in there because every figure below scales with
 * it - an RP2350 at 150 MHz against an RP2040 at 125 MHz is not a fair
 * comparison of the two cores unless the clock is on the page.
 */
#define BENCH_STR2(x)       #x
#define BENCH_STR(x)        BENCH_STR2(x)

#ifndef BENCH_GIT
#define BENCH_GIT           "unknown"
#endif
#ifndef BENCH_BUILT
#define BENCH_BUILT         "unknown"
#endif

#if defined(DEVICE_BOARD_NAME)
#define BENCH_BOARD         BENCH_STR(DEVICE_BOARD_NAME)
#else
#define BENCH_BOARD         "unknown"
#endif

#if defined(PICO_RP2350) && PICO_RP2350
#define BENCH_PART          "RP2350 (Cortex-M33)"
#elif defined(PICO_RP2040) && PICO_RP2040
#define BENCH_PART          "RP2040 (Cortex-M0+)"
#else
#define BENCH_PART          "unknown part"
#endif

#if defined(MBEDTLS_HAVE_ASM)
#define BENCH_BIGNUM        "assembly (MBEDTLS_HAVE_ASM)"
#else
#define BENCH_BIGNUM        "C (MBEDTLS_HAVE_ASM off)"
#endif

/*
 * The ECC rows are meaningless without this line. The first ECC measurements
 * were taken with NIST_OPTIM off by accident, and nothing in the output said
 * so - a later reader would have compared two different configurations.
 */
#if defined(MBEDTLS_ECP_NIST_OPTIM)
#define BENCH_ECP           "NIST_OPTIM on"
#else
#define BENCH_ECP           "NIST_OPTIM off (generic reduction)"
#endif

/*
 * Where the code runs from. The SDK defines this either way, 0 for an ordinary
 * flash image, so the banner can state it rather than leaving it to be inferred
 * from the build command.
 */
#if defined(PICO_COPY_TO_RAM) && PICO_COPY_TO_RAM
#define BENCH_XIP           "RAM (copy_to_ram, no XIP cache)"
#else
#define BENCH_XIP           "flash (XIP)"
#endif

/*
 * One OAEP block, as the policy computes it: the RSA modulus minus 42 bytes of
 * OAEP overhead (two SHA-1 digests plus a byte, rounded as the policy does).
 * Measuring with a 32-byte payload would time the same modular exponentiation
 * but hide the fact that a real OPN needs more than one of them.
 */
#define OAEP_PLAIN_BLOCK    214

/*
 * How many blocks a normal OPN takes in each direction.
 *
 * The encrypted part is SequenceHeader + body + padding + signature, and the
 * signature alone is 256 bytes with an RSA-2048 key. Two is the floor for both
 * sides when both certificates are RSA-2048; a larger endpoint URL or a longer
 * certificate pushes it to three.
 */
#define OPN_REQUEST_BLOCKS  2
#define OPN_RESPONSE_BLOCKS 2

/*
 * P-SHA256 key derivation calls HMAC once per output block, in both directions.
 * Each direction needs a 32-byte signing key, a 32-byte encryption key and a
 * 16-byte IV, and the running A(i) chain costs a call of its own - which comes
 * to fourteen calls for the pair, not the one the first version measured.
 */
#define KEYDERIVE_HMACS     14

static uint8_t s_buf[THROUGHPUT_BYTES];
static uint8_t s_out[THROUGHPUT_BYTES + 16];

/* ------------------------------------------------------------------- rng */

/*
 * Not random, and deliberately so: a benchmark that drew from a real entropy
 * source would be timing the entropy source as well. Every byte is produced by
 * the same arithmetic on every run, so two runs of the same firmware are
 * comparable to each other.
 */
static int bench_rng(void *ctx, unsigned char *out, size_t len)
{
    static uint32_t s = 0x12345678u;

    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        out[i] = (unsigned char)s;
    }
    return 0;
}

/* ----------------------------------------------------------------- timing */

typedef struct {
    const char *name;
    uint32_t    runs;
    uint64_t    total_us;
    uint64_t    min_us;
    int         failed;
} result_t;

#define MAX_RESULTS 24
static result_t s_res[MAX_RESULTS];
static int      s_n_res;

static result_t *result_begin(const char *name)
{
    result_t *r = &s_res[s_n_res++];

    r->name     = name;
    r->runs     = 0;
    r->total_us = 0;
    r->min_us   = UINT64_MAX;
    r->failed   = 0;
    return r;
}

static void result_add(result_t *r, uint64_t us, int rc)
{
    r->runs++;
    r->total_us += us;
    if (us < r->min_us) r->min_us = us;
    if (rc != 0) r->failed = rc;
}

/*
 * Progress as it goes, not only at the end.
 *
 * An RSA private operation on this part may take a second or more, and a table
 * that appears after a minute of silence is indistinguishable from a board that
 * has hung - which, during the operation, is very nearly what it is.
 */
static void result_report(const result_t *r)
{
    double avg = (double)r->total_us / (double)r->runs;

    if (r->failed) {
        printf("  %-26s FAILED  rc=-0x%04x\n", r->name, (unsigned)-r->failed);
        return;
    }

    if (avg >= 1000.0) {
        printf("  %-26s %8.2f ms   (min %7.2f ms, %lu runs)\n",
               r->name, avg / 1000.0, (double)r->min_us / 1000.0,
               (unsigned long)r->runs);
    } else {
        printf("  %-26s %8.1f us   (min %7.1f us, %lu runs)\n",
               r->name, avg, (double)r->min_us, (unsigned long)r->runs);
    }
}

/* ------------------------------------------------------------------ bench */

static mbedtls_pk_context  s_pk;
static mbedtls_rsa_context *s_rsa;

static uint8_t s_hash[32];
static uint8_t s_sig[256];
static uint8_t s_cipher[256];
static uint8_t s_plain[OAEP_PLAIN_BLOCK];

static void bench_rsa_private(void)
{
    result_t *sign = result_begin("RSA-2048 sign (private)");
    result_t *dec  = result_begin("RSA-2048 OAEP dec (priv)");
    size_t    olen;
    uint8_t   plain[OAEP_PLAIN_BLOCK];

    /*
     * PKCS#1 v1.5 for the signature and OAEP for the key transport, which is
     * what Basic256Sha256 specifies. The OAEP hash is SHA-1, not SHA-256: the
     * policy's name refers to the signature and the symmetric side, and the
     * MGF1 inside OAEP stayed on SHA-1. Getting this wrong produces a server
     * that interoperates with nothing.
     */
    for (int i = 0; i < 5; i++) {
        mbedtls_rsa_set_padding(s_rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_SHA256);

        uint64_t t0 = time_us_64();
        int rc = mbedtls_rsa_rsassa_pkcs1_v15_sign(s_rsa, bench_rng, NULL,
                                                   MBEDTLS_MD_SHA256,
                                                   32, s_hash, s_sig);
        result_add(sign, time_us_64() - t0, rc);

        mbedtls_rsa_set_padding(s_rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);

        t0 = time_us_64();
        rc = mbedtls_rsa_rsaes_oaep_decrypt(s_rsa, bench_rng, NULL, NULL, 0,
                                            &olen, s_cipher, plain,
                                            sizeof(plain));
        result_add(dec, time_us_64() - t0, rc);

        /* A decrypt that returned the wrong length is a decrypt that measured
         * the wrong thing. */
        if (rc == 0 && olen != OAEP_PLAIN_BLOCK) {
            printf("    WARNING: decrypt returned %u bytes, expected %d\n",
                   (unsigned)olen, OAEP_PLAIN_BLOCK);
        }

        printf("    run %d/5 done\n", i + 1);
    }

    result_report(sign);
    result_report(dec);
}

static void bench_rsa_public(void)
{
    result_t *ver = result_begin("RSA-2048 verify (public)");
    result_t *enc = result_begin("RSA-2048 OAEP enc (pub)");

    for (int i = 0; i < 10; i++) {
        mbedtls_rsa_set_padding(s_rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_SHA256);

        uint64_t t0 = time_us_64();
        int rc = mbedtls_rsa_rsassa_pkcs1_v15_verify(s_rsa, MBEDTLS_MD_SHA256,
                                                     32, s_hash, s_sig);
        result_add(ver, time_us_64() - t0, rc);

        mbedtls_rsa_set_padding(s_rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);

        t0 = time_us_64();
        rc = mbedtls_rsa_rsaes_oaep_encrypt(s_rsa, bench_rng, NULL, NULL, 0,
                                            OAEP_PLAIN_BLOCK, s_plain,
                                            s_cipher);
        result_add(enc, time_us_64() - t0, rc);
    }

    result_report(ver);
    result_report(enc);
}

static void bench_parsing(void)
{
    result_t *pk  = result_begin("parse private key (DER)");
    result_t *crt = result_begin("parse certificate");
    size_t klen, clen;
    const unsigned char *key  = bench_key_der(&klen);
    const unsigned char *cert = bench_cert_der(&clen);

    for (int i = 0; i < 5; i++) {
        mbedtls_pk_context tmp;
        mbedtls_pk_init(&tmp);

        uint64_t t0 = time_us_64();
        int rc = mbedtls_pk_parse_key(&tmp, key, klen, NULL, 0,
                                      bench_rng, NULL);
        result_add(pk, time_us_64() - t0, rc);
        mbedtls_pk_free(&tmp);
    }

    for (int i = 0; i < 10; i++) {
        mbedtls_x509_crt c;
        mbedtls_x509_crt_init(&c);

        uint64_t t0 = time_us_64();
        int rc = mbedtls_x509_crt_parse_der(&c, cert, clen);
        result_add(crt, time_us_64() - t0, rc);
        mbedtls_x509_crt_free(&c);
    }

    /*
     * Verification, which is what the server actually does to an arriving
     * certificate - parsing only reads the bytes. The signature check inside is
     * another public-key operation and belongs in the total; treating a verify
     * as a parse understated an OPN by one RSA operation.
     */
    result_t *vfy = result_begin("verify certificate chain");

    for (int i = 0; i < 5; i++) {
        mbedtls_x509_crt c, trust;
        uint32_t flags = 0;

        mbedtls_x509_crt_init(&c);
        mbedtls_x509_crt_init(&trust);
        mbedtls_x509_crt_parse_der(&c, cert, clen);
        mbedtls_x509_crt_parse_der(&trust, cert, clen);

        uint64_t t0 = time_us_64();
        int rc = mbedtls_x509_crt_verify(&c, &trust, NULL, NULL, &flags,
                                         NULL, NULL);
        result_add(vfy, time_us_64() - t0, rc);

        mbedtls_x509_crt_free(&c);
        mbedtls_x509_crt_free(&trust);
    }

    result_report(pk);
    result_report(crt);
    result_report(vfy);
}

static void bench_symmetric(void)
{
    result_t *sha  = result_begin("SHA-256 8 KB");
    result_t *aes  = result_begin("AES-256-CBC 8 KB");
    result_t *hmac = result_begin("HMAC-SHA256 32 B");

    uint8_t key[32], iv[16], out[32];

    memset(key, 0xA5, sizeof(key));
    memset(iv,  0x5A, sizeof(iv));

    for (int i = 0; i < 20; i++) {
        uint64_t t0 = time_us_64();
        int rc = mbedtls_sha256(s_buf, sizeof(s_buf), out, 0);
        result_add(sha, time_us_64() - t0, rc);
    }

    for (int i = 0; i < 20; i++) {
        mbedtls_aes_context a;
        uint8_t ivc[16];

        mbedtls_aes_init(&a);
        mbedtls_aes_setkey_enc(&a, key, 256);
        memcpy(ivc, iv, sizeof(ivc));

        uint64_t t0 = time_us_64();
        int rc = mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_ENCRYPT,
                                       sizeof(s_buf), ivc, s_buf, s_out);
        result_add(aes, time_us_64() - t0, rc);
        mbedtls_aes_free(&a);
    }

    for (int i = 0; i < 50; i++) {
        const mbedtls_md_info_t *info =
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

        uint64_t t0 = time_us_64();
        int rc = mbedtls_md_hmac(info, key, sizeof(key), s_buf, 32, out);
        result_add(hmac, time_us_64() - t0, rc);
    }

    /*
     * HMAC over a full buffer, not only the 32-byte case.
     *
     * Basic256Sha256 signs every message as well as encrypting it, so the real
     * per-message cost is AES over the payload PLUS HMAC over the payload. A
     * secure-throughput figure built from the AES row alone is optimistic by
     * roughly a factor of two, which is exactly the mistake this row exists to
     * stop. The 32-byte row above is the fixed overhead; this one is what
     * signing a full message actually costs.
     */
    result_t *hmac8k = result_begin("HMAC-SHA256 8 KB");

    for (int i = 0; i < 20; i++) {
        const mbedtls_md_info_t *info =
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

        uint64_t t0 = time_us_64();
        int rc = mbedtls_md_hmac(info, key, sizeof(key),
                                 s_buf, sizeof(s_buf), out);
        result_add(hmac8k, time_us_64() - t0, rc);
    }

    /* The whole P-SHA256 derivation, both directions, as the policy runs it. */
    result_t *kd = result_begin("key derivation (14 HMAC)");

    for (int i = 0; i < 10; i++) {
        const mbedtls_md_info_t *info =
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        int rc = 0;

        uint64_t t0 = time_us_64();
        for (int h = 0; h < KEYDERIVE_HMACS; h++) {
            rc |= mbedtls_md_hmac(info, key, sizeof(key), s_buf, 64, out);
        }
        result_add(kd, time_us_64() - t0, rc);
    }

    result_report(sha);
    result_report(aes);
    result_report(hmac);
    result_report(hmac8k);
    result_report(kd);
}

/*
 * The entropy source, which everything above quietly assumes.
 *
 * The benchmark's own RNG is a counter, so none of the timings here include the
 * cost of real randomness - but a handshake does, twice over: seeding the DRBG
 * at startup and drawing from it per signature. Two things are worth knowing
 * before any of that ships.
 *
 * How long seeding takes, because it happens inside the window a client is
 * waiting on, and the handshake totals above exclude it.
 *
 * What the source actually emits. A block of raw output is printed so it can be
 * captured from the serial port and examined offline. That matters most on the
 * RP2040, which has no TRNG: pico_rand falls back to the ring oscillator there,
 * and nobody has shown that output is good enough to carry an ECDSA nonce. A
 * repeated or predictable nonce does not weaken the signature, it publishes the
 * private key. Printing the bytes is not a test; it is what makes a test
 * possible.
 */
static void bench_entropy(void)
{
    result_t *seed = result_begin("CTR-DRBG seed");
    result_t *draw = result_begin("entropy 1 KB");

    printf("  source   %s\n", wiz_entropy_source_name());

    for (int i = 0; i < 5; i++) {
        mbedtls_entropy_context  ent;
        mbedtls_ctr_drbg_context drbg;

        mbedtls_entropy_init(&ent);
        mbedtls_ctr_drbg_init(&drbg);

        uint64_t t0 = time_us_64();
        int rc = wiz_entropy_seed_drbg(&drbg, &ent, "crypto_bench");
        result_add(seed, time_us_64() - t0, rc);

        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&ent);
    }

    for (int i = 0; i < 10; i++) {
        uint64_t t0 = time_us_64();
        int rc = wiz_entropy_raw(s_buf, 1024);
        result_add(draw, time_us_64() - t0, rc);
    }

    result_report(seed);
    result_report(draw);

    /* 32 bytes, for reading rather than for using. */
    unsigned char sample[32];
    if (wiz_entropy_raw(sample, sizeof(sample)) == 0) {
        printf("  raw      ");
        for (size_t i = 0; i < sizeof(sample); i++) {
            printf("%02x", sample[i]);
        }
        printf("\n");
        printf("  (one sample proves nothing - capture many and test offline)\n");
    }

    /* s_buf was overwritten above; the symmetric rows want it filled again. */
    memset(s_buf, 0x42, sizeof(s_buf));
}

/*
 * The same handshake with elliptic curves instead of RSA.
 *
 * This exists because the RSA figures closed a door that ECC may leave open.
 * An OPC UA ECC policy (securitypolicy_eccnistp256.c, already in the tree) and
 * a TLS handshake both need the same operations once per connection: an
 * ephemeral key pair, one key agreement, one signature with our own key, and
 * one verification of the peer's. P-256 private operations are a different
 * order of magnitude from RSA-2048 ones, so a part that cannot finish an RSA
 * handshake inside a client's timeout may still finish an ECC one.
 *
 * There is a second reason to measure it. mbedTLS has a restartable ECP path
 * (MBEDTLS_ECP_RESTARTABLE, off in our config) and no RSA equivalent, so with
 * ECC the stall can be split across loop iterations instead of blocking the
 * whole cooperative loop. Neither point is worth arguing about without numbers.
 *
 * The cost of ECC is interoperability, not arithmetic: a client has to offer
 * the policy. That is a separate question from this one.
 */
static void bench_ecc(void)
{
    result_t *kg = result_begin("ECDH P-256 keygen");
    result_t *dh = result_begin("ECDH P-256 agree");
    result_t *sg = result_begin("ECDSA P-256 sign");
    result_t *vr = result_begin("ECDSA P-256 verify");

    mbedtls_ecp_group grp;
    mbedtls_mpi d, dpeer, z;
    mbedtls_ecp_point Q, Qpeer;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&dpeer);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&Q);
    mbedtls_ecp_point_init(&Qpeer);

    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) {
        printf("  ECC                        FAILED  group load -0x%04x\n",
               (unsigned)-rc);
        return;
    }

    /* The peer's key, generated once - we are timing our own side only. */
    mbedtls_ecp_gen_keypair(&grp, &dpeer, &Qpeer, bench_rng, NULL);

    for (int i = 0; i < 10; i++) {
        mbedtls_mpi_free(&d);
        mbedtls_ecp_point_free(&Q);
        mbedtls_mpi_init(&d);
        mbedtls_ecp_point_init(&Q);

        uint64_t t0 = time_us_64();
        rc = mbedtls_ecp_gen_keypair(&grp, &d, &Q, bench_rng, NULL);
        result_add(kg, time_us_64() - t0, rc);
    }

    for (int i = 0; i < 10; i++) {
        uint64_t t0 = time_us_64();
        rc = mbedtls_ecdh_compute_shared(&grp, &z, &Qpeer, &d,
                                         bench_rng, NULL);
        result_add(dh, time_us_64() - t0, rc);
    }

    /* Signed over the same 32-byte digest the RSA rows use, for comparability. */
    mbedtls_ecdsa_context ecdsa;
    mbedtls_ecdsa_init(&ecdsa);

    if (mbedtls_ecdsa_genkey(&ecdsa, MBEDTLS_ECP_DP_SECP256R1,
                             bench_rng, NULL) == 0) {
        unsigned char sig[MBEDTLS_ECDSA_MAX_LEN];
        size_t slen = 0;

        for (int i = 0; i < 10; i++) {
            uint64_t t0 = time_us_64();
            rc = mbedtls_ecdsa_write_signature(&ecdsa, MBEDTLS_MD_SHA256,
                                               s_hash, 32,
                                               sig, sizeof(sig), &slen,
                                               bench_rng, NULL);
            result_add(sg, time_us_64() - t0, rc);
        }

        for (int i = 0; i < 10; i++) {
            uint64_t t0 = time_us_64();
            rc = mbedtls_ecdsa_read_signature(&ecdsa, s_hash, 32, sig, slen);
            result_add(vr, time_us_64() - t0, rc);
        }
    }

    mbedtls_ecdsa_free(&ecdsa);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_point_free(&Qpeer);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&dpeer);
    mbedtls_mpi_free(&z);
    mbedtls_ecp_group_free(&grp);

    result_report(kg);
    result_report(dh);
    result_report(sg);
    result_report(vr);
}

/*
 * The first private operation after a key is set up, with nothing cached.
 *
 * mbedTLS keeps the blinding values it computed last time inside the RSA context
 * and reuses them, so every measurement after the first is cheaper than the
 * first. A server meets the expensive version once per policy setup, and a
 * benchmark that only ever reports the warm number quietly flatters itself.
 *
 * Re-parsing the key each round is what makes it cold: a fresh context has no
 * cached blinding to reuse.
 */
static void bench_cold_start(void)
{
    result_t *cold = result_begin("first sign after key load");
    size_t klen;
    const unsigned char *key = bench_key_der(&klen);

    for (int i = 0; i < 3; i++) {
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);

        if (mbedtls_pk_parse_key(&pk, key, klen, NULL, 0, bench_rng, NULL) != 0) {
            mbedtls_pk_free(&pk);
            continue;
        }

        mbedtls_rsa_context *r = mbedtls_pk_rsa(pk);
        mbedtls_rsa_set_padding(r, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_SHA256);

        uint64_t t0 = time_us_64();
        int rc = mbedtls_rsa_rsassa_pkcs1_v15_sign(r, bench_rng, NULL,
                                                   MBEDTLS_MD_SHA256,
                                                   32, s_hash, s_sig);
        result_add(cold, time_us_64() - t0, rc);
        mbedtls_pk_free(&pk);

        printf("    cold run %d/3 done\n", i + 1);
    }

    result_report(cold);
}

/* ------------------------------------------------------------------- main */

static const result_t *find(const char *name)
{
    for (int i = 0; i < s_n_res; i++) {
        if (strcmp(s_res[i].name, name) == 0) return &s_res[i];
    }
    return NULL;
}

static double avg_ms(const char *name)
{
    const result_t *r = find(name);

    if (!r || r->runs == 0 || r->failed) return -1.0;
    return ((double)r->total_us / (double)r->runs) / 1000.0;
}

static void verdict(void)
{
    double sign = avg_ms("RSA-2048 sign (private)");
    double dec  = avg_ms("RSA-2048 OAEP dec (priv)");
    double enc  = avg_ms("RSA-2048 OAEP enc (pub)");
    double ver  = avg_ms("RSA-2048 verify (public)");
    double vfy  = avg_ms("verify certificate chain");
    double pk   = avg_ms("parse private key (DER)");
    double kd   = avg_ms("key derivation (14 HMAC)");

    if (sign < 0 || dec < 0 || enc < 0 || ver < 0 || vfy < 0 || kd < 0) {
        printf("\n  (no verdict - an operation failed or did not run)\n");
        return;
    }

    double rsa = OPN_REQUEST_BLOCKS * dec     /* open the request          */
               + ver                          /* its signature            */
               + sign                         /* sign the response        */
               + OPN_RESPONSE_BLOCKS * enc    /* seal the response        */
               + vfy;                         /* the client's certificate */

    double total = rsa + pk + kd;

    printf("\n  OpenSecureChannel, server side, cryptography only\n");
    printf("    %d x OAEP decrypt (private)   %8.0f ms\n",
           OPN_REQUEST_BLOCKS, OPN_REQUEST_BLOCKS * dec);
    printf("    1 x verify request signature  %8.0f ms\n", ver);
    printf("    1 x sign response (private)   %8.0f ms\n", sign);
    printf("    %d x OAEP encrypt (public)    %8.0f ms\n",
           OPN_RESPONSE_BLOCKS, OPN_RESPONSE_BLOCKS * enc);
    printf("    1 x verify certificate        %8.0f ms\n", vfy);
    printf("    key parse + derivation        %8.0f ms\n", pk + kd);
    printf("                                  --------\n");
    printf("                                  %8.0f ms\n", total);

    printf("\n  What this figure is NOT:\n");
    printf("    It is the arithmetic only. A real connection also carries\n");
    printf("    round trips, encoding, PKI list walking, DRBG seeding,\n");
    printf("    allocation and Ethernet transfer, so the time a client sees\n");
    printf("    is longer than this. Treat it as a floor.\n");
    printf("    Block counts assume RSA-2048 on both sides and a small OPN;\n");
    printf("    a longer endpoint URL or certificate makes it three.\n");

    printf("\n  While this runs, the server's cooperative loop does not:\n");
    printf("    other sessions are not served and the watchdog is not kicked.\n");
    printf("    Interrupt-driven work such as USB does keep running.\n");

    printf("\n  open62541's own client gives up after 5000 ms\n");
    printf("    (plugins/ua_config_default.c: config->timeout = 5 * 1000)\n");
    printf("    margin here: %.0f ms\n", 5000.0 - total);
}

/*
 * Two figures the RSA verdict above does not contain, kept separate from it so
 * that they still print when an RSA row fails.
 *
 * The first is what a message costs once the channel is open. SignAndEncrypt
 * means both AES and HMAC over the payload, so quoting the AES row on its own
 * overstates the achievable rate by about a factor of two. Even the sum here is
 * a ceiling: it counts the two primitives and nothing else - no chunk framing,
 * padding, copies, SPI transfer or TOE service time - so an end-to-end number
 * measured through the stack will be lower, and that is the number a datasheet
 * may quote.
 *
 * The second is the same handshake under an ECC policy. It decides whether the
 * RSA result closes the door for a part or only for one policy.
 */
static void report_alternatives(void)
{
    double aes8 = avg_ms("AES-256-CBC 8 KB");
    double hm8  = avg_ms("HMAC-SHA256 8 KB");
    double kd   = avg_ms("key derivation (14 HMAC)");

    if (aes8 >= 0 && hm8 >= 0) {
        double ms = aes8 + hm8;

        printf("\n  Per-message cost of SignAndEncrypt, %d B payload\n",
               THROUGHPUT_BYTES);
        printf("    AES-256-CBC                   %8.2f ms\n", aes8);
        printf("    HMAC-SHA256                   %8.2f ms\n", hm8);
        printf("                                  --------\n");
        printf("    %8.2f ms  =  %.2f Mbit/s of payload (primitives only)\n",
               ms, (THROUGHPUT_BYTES * 8.0) / (ms * 1000.0));
        printf("    Not a link rate and not an application rate. The AES row\n");
        printf("    alone would read about %.2f Mbit/s, which is why it must\n",
               (THROUGHPUT_BYTES * 8.0) / (aes8 * 1000.0));
        printf("    never be quoted as a secure throughput on its own.\n");
    }

    double ekg = avg_ms("ECDH P-256 keygen");
    double edh = avg_ms("ECDH P-256 agree");
    double esg = avg_ms("ECDSA P-256 sign");
    double evr = avg_ms("ECDSA P-256 verify");

    if (ekg < 0 || edh < 0 || esg < 0 || evr < 0) {
        printf("\n  (no ECC comparison - an operation failed or did not run)\n");
        return;
    }

    double ecc = ekg + edh + esg + evr + (kd >= 0 ? kd : 0.0);

    printf("\n  The same handshake under an ECC policy (P-256)\n");
    printf("    1 x ephemeral keygen          %8.2f ms\n", ekg);
    printf("    1 x ECDH agree                %8.2f ms\n", edh);
    printf("    1 x ECDSA sign (our key)      %8.2f ms\n", esg);
    printf("    1 x ECDSA verify (peer)       %8.2f ms\n", evr);
    if (kd >= 0) {
        printf("    key derivation                %8.2f ms\n", kd);
    }
    printf("                                  --------\n");
    printf("                                  %8.2f ms\n", ecc);
    printf("    margin against the 5000 ms client timeout: %.0f ms\n",
           5000.0 - ecc);

    /*
     * Compared against the whole RSA handshake, not only its private
     * operations, because the ECC figure above also includes verifying the
     * peer. The direction is printed explicitly: the first version of this line
     * said "1x cheaper" for a ratio of 0.69, which read as a win when ECC was
     * in fact the more expensive of the two.
     */
    double sign = avg_ms("RSA-2048 sign (private)");
    double dec  = avg_ms("RSA-2048 OAEP dec (priv)");
    double enc  = avg_ms("RSA-2048 OAEP enc (pub)");
    double ver  = avg_ms("RSA-2048 verify (public)");
    double vfy  = avg_ms("verify certificate chain");
    double pkp  = avg_ms("parse private key (DER)");

    if (sign >= 0 && dec >= 0 && enc >= 0 && ver >= 0 && vfy >= 0 && ecc > 0.0) {
        double rsa = OPN_REQUEST_BLOCKS * dec + ver + sign
                   + OPN_RESPONSE_BLOCKS * enc + vfy
                   + (pkp >= 0 ? pkp : 0.0) + (kd >= 0 ? kd : 0.0);

        if (ecc < rsa) {
            printf("    against the RSA handshake (%.0f ms): %.2fx cheaper\n",
                   rsa, rsa / ecc);
        } else {
            printf("    against the RSA handshake (%.0f ms): %.2fx MORE"
                   " expensive\n", rsa, ecc / rsa);
            printf("    On this part ECC is not the cheaper policy. Check the\n");
            printf("    ecp line in the banner before reading anything into it.\n");
        }
    }

    printf("\n  What this does NOT settle: whether the clients in the field\n");
    printf("    offer an ECC policy at all. That is interoperability, not\n");
    printf("    arithmetic, and it has to be checked separately.\n");
}

static void bench_banner(void)
{
    printf("\n================ crypto_bench ================\n");
    printf("  board    %s\n", BENCH_BOARD);
    printf("  part     %s\n", BENCH_PART);
    printf("  clk_sys  %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));
    printf("  bignum   %s\n", BENCH_BIGNUM);
    printf("  ecp      %s\n", BENCH_ECP);
    printf("  code     %s\n", BENCH_XIP);
    printf("  git      %s\n", BENCH_GIT);
    printf("  built    %s\n", BENCH_BUILT);
    printf("==============================================\n");
}

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);

    bench_banner();

    memset(s_buf, 0x42, sizeof(s_buf));
    memset(s_hash, 0x11, sizeof(s_hash));

    size_t klen;
    const unsigned char *key = bench_key_der(&klen);

    mbedtls_pk_init(&s_pk);
    int rc = mbedtls_pk_parse_key(&s_pk, key, klen, NULL, 0, bench_rng, NULL);
    if (rc != 0) {
        while (true) {
            printf("FATAL: key parse failed, rc=-0x%04x\n", (unsigned)-rc);
            sleep_ms(2000);
        }
    }
    s_rsa = mbedtls_pk_rsa(s_pk);

    /*
     * One sealed block to decrypt over and over, a full 214 bytes rather than a
     * token 32. The modular exponentiation costs the same either way, but using
     * the real block size keeps the file honest about what one block holds.
     */
    memset(s_plain, 0x7E, sizeof(s_plain));
    mbedtls_rsa_set_padding(s_rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
    rc = mbedtls_rsa_rsaes_oaep_encrypt(s_rsa, bench_rng, NULL, NULL, 0,
                                        OAEP_PLAIN_BLOCK, s_plain, s_cipher);
    if (rc != 0) {
        while (true) {
            printf("FATAL: OAEP seal failed, rc=-0x%04x\n", (unsigned)-rc);
            sleep_ms(2000);
        }
    }

    /* Signed once up front so the verify benchmark has something real. */
    mbedtls_rsa_set_padding(s_rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_SHA256);
    mbedtls_rsa_rsassa_pkcs1_v15_sign(s_rsa, bench_rng, NULL,
                                      MBEDTLS_MD_SHA256, 32, s_hash, s_sig);

    while (true) {
        s_n_res = 0;

        printf("\n================================================\n");
        printf("RSA / OPC UA crypto cost on this board\n");
        printf("  board      " PICO_BOARD "\n");
        printf("  sys clock  %lu Hz\n",
               (unsigned long)clock_get_hz(clk_sys));
        printf("  key        RSA-2048, mbedTLS software only\n");
        printf("================================================\n\n");

        printf("  cold start - no cached blinding\n");
        bench_cold_start();

        printf("\n  private key operations (the expensive half)\n");
        bench_rsa_private();

        printf("\n  public key operations\n");
        bench_rsa_public();

        printf("\n  parsing, once per start / per client\n");
        bench_parsing();

        printf("\n  per-message symmetric work\n");
        bench_symmetric();

        printf("\n  the same handshake with ECC instead of RSA\n");
        bench_ecc();

        printf("\n  entropy - the floor everything above stands on\n");
        bench_entropy();

        verdict();
        report_alternatives();

        printf("\n  repeating in 30 s\n");
        sleep_ms(30000);
    }
}
