/**
 * @file    wiz_entropy.c
 * @brief   mbedTLS entropy source backed by pico_rand.
 *
 * See wiz_entropy.h for why this exists and what it deliberately does not
 * claim. Lifted from port/wiz-claw/src/wiz_claw_tls.c, which has been using the
 * same callback on hardware; the logic is unchanged apart from the part check
 * and the raw-output entry point.
 */

#include <string.h>

#include "pico/rand.h"

/* Tells the header this is the implementation, so the caller-side part check
 * does not stop the library from building on a part without a TRNG. */
#define WIZ_ENTROPY_IMPL 1
#include "wiz_entropy.h"

/*
 * pico_rand's output, not the raw generator.
 *
 * get_rand_32() returns the output of pico_rand's generator, which on the
 * RP2350 mixes a fresh TRNG sample into its state on every call. That is a
 * deliberate choice over reading the TRNG registers directly: the SDK already
 * handles the start-up, sampling and decorrelation sequence, and a hand-rolled
 * register loop here would be a second implementation of it to keep correct.
 *
 * The cost of the choice is that the bytes below are generator output rather
 * than raw noise, so the strength claim rests on pico_rand feeding it real
 * entropy - true on the RP2350, unestablished on the RP2040.
 */
static int wiz_entropy_poll(void          *data,
                            unsigned char *output,
                            size_t         len,
                            size_t        *olen)
{
    (void)data;

    int rc = wiz_entropy_raw(output, len);
    if (rc != 0) {
        *olen = 0;
        return rc;
    }

    *olen = len;
    return 0;
}

int wiz_entropy_raw(unsigned char *out, size_t len)
{
    if (out == NULL) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    size_t i = 0;

    for (; i + sizeof(uint32_t) <= len; i += sizeof(uint32_t)) {
        uint32_t r = get_rand_32();
        memcpy(out + i, &r, sizeof(uint32_t));
    }

    if (i < len) {
        uint32_t r = get_rand_32();
        memcpy(out + i, &r, len - i);
    }

    return 0;
}

/*
 * The platform source mbedTLS adds to every entropy context by itself.
 *
 * wiz_entropy_register() only works on a context we own. open62541's security
 * policies own theirs: securitypolicy_basic256sha256.c:526 and
 * securitypolicy_eccnistp256.c:70 each call mbedtls_entropy_init() on a context
 * inside the policy and seed a DRBG from it, with no way in from outside. With
 * MBEDTLS_NO_PLATFORM_ENTROPY set and no source registered, that seeding fails
 * and the policy does not initialise - no secure endpoint at all.
 *
 * MBEDTLS_ENTROPY_HARDWARE_ALT makes mbedtls_entropy_init() add this function
 * as a strong source on every context, including the ones we never see. Both
 * mechanisms are needed and they are not alternatives: this one for contexts
 * owned by library code, wiz_entropy_register() for ours.
 */
int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen)
{
    (void)data;

    int rc = wiz_entropy_raw(output, len);
    if (rc != 0) {
        *olen = 0;
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    *olen = len;
    return 0;
}

int wiz_entropy_register(mbedtls_entropy_context *entropy)
{
    if (entropy == NULL) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    /*
     * 16 bytes of threshold is what mbedtls_entropy_func will insist on
     * collecting from this source before it will hand anything back, and
     * SOURCE_STRONG says one source is enough. Both match the existing
     * wiz_claw_tls usage, so the two paths behave the same way.
     */
    return mbedtls_entropy_add_source(entropy, wiz_entropy_poll, NULL,
                                      16, MBEDTLS_ENTROPY_SOURCE_STRONG);
}

int wiz_entropy_seed_drbg(mbedtls_ctr_drbg_context *drbg,
                          mbedtls_entropy_context  *entropy,
                          const char               *label)
{
    if (drbg == NULL || entropy == NULL) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

    int rc = wiz_entropy_register(entropy);
    if (rc != 0) {
        return rc;
    }

    const char *pers = (label != NULL) ? label : "wiznet-pico-c";

    return mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, entropy,
                                 (const unsigned char *)pers, strlen(pers));
}

const char *wiz_entropy_source_name(void)
{
#if WIZ_ENTROPY_HAS_TRNG
    return "pico_rand (RP2350 TRNG mixed per call)";
#else
    return "pico_rand (ring oscillator - quality not established)";
#endif
}
