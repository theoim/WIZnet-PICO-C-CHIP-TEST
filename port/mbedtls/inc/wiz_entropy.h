/**
 * @file    wiz_entropy.h
 * @brief   An entropy source for mbedTLS on this family of parts.
 *
 * Every private-key operation needs randomness, and two of them need it to be
 * unpredictable or the key is lost rather than merely weakened: RSA blinding,
 * and the per-signature value in ECDSA. A repeated or guessable ECDSA nonce
 * reveals the private key outright. So this is not a quality-of-service knob;
 * it is the floor the rest of the security sits on.
 *
 * The working implementation this is lifted from lives in
 * port/wiz-claw/src/wiz_claw_tls.c, where it is a file-static callback bound to
 * a TLS client context. That made it unreachable from the OPC UA server without
 * dragging in cJSON and the rest of the wiz_claw library, hence this module.
 *
 * What it does NOT provide: a key store. The private key still lives in flash
 * and in RAM, readable by anything running on the part. A secure element keeps
 * the key inside a certified boundary and signs on your behalf; a hardware
 * random generator does not, and the two must not be confused when describing
 * what this part offers.
 */

#ifndef WIZ_ENTROPY_H
#define WIZ_ENTROPY_H

#include <stddef.h>
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Whether this part has a hardware random number generator.
 *
 * The RP2350 has a TRNG and pico_rand mixes it in on every call
 * (HAS_RP2350_TRNG in rp2350/hardware_regs/.../platform_defs.h, and
 * PICO_RAND_ENTROPY_SRC_TRNG defaulting to 1 in pico_rand/include/pico/rand.h).
 * The RP2040 has none: pico_rand there derives its state from the ring
 * oscillator, timers and bus counters, which is a different and unmeasured
 * quality claim.
 */
#if defined(PICO_RP2350) && PICO_RP2350
#define WIZ_ENTROPY_HAS_TRNG 1
#else
#define WIZ_ENTROPY_HAS_TRNG 0
#endif

/*
 * On a part with no TRNG the build stops here unless the caller says, in the
 * build, that it accepts a ring-oscillator-derived source. The point is that
 * the decision has to be made by someone rather than inherited silently: this
 * source is registered as MBEDTLS_ENTROPY_SOURCE_STRONG, which tells mbedTLS
 * its output is full entropy, and on the RP2040 nobody has shown that yet.
 *
 * Define WIZ_ENTROPY_ALLOW_NO_TRNG to proceed - and then owe the file a
 * measurement, not an assumption.
 *
 * The gate is on the CALLER, not on the implementation: wiz_entropy.c sets
 * WIZ_ENTROPY_IMPL and compiles for every part, because it is built as its own
 * library target and would otherwise never see a consumer's definition. The
 * acknowledgement is a decision someone has to write down in their own build,
 * which is the whole point of it.
 */
#if !WIZ_ENTROPY_HAS_TRNG && \
    !defined(WIZ_ENTROPY_ALLOW_NO_TRNG) && !defined(WIZ_ENTROPY_IMPL)
#error "This part has no hardware RNG. Define WIZ_ENTROPY_ALLOW_NO_TRNG to use the ring-oscillator source deliberately, and document why it is adequate."
#endif

/** Registers the platform source on an initialised entropy context. */
int wiz_entropy_register(mbedtls_entropy_context *entropy);

/**
 * Registers the source and seeds a CTR-DRBG from it in one step.
 *
 * Both contexts must already be initialised. @p label is mixed in as a
 * personalisation string, so two devices running different firmware do not
 * produce the same stream from the same entropy; pass something naming the
 * application, not a secret.
 */
int wiz_entropy_seed_drbg(mbedtls_ctr_drbg_context *drbg,
                          mbedtls_entropy_context  *entropy,
                          const char               *label);

/**
 * Fills @p out with raw source output, bypassing mbedTLS.
 *
 * For judging the source rather than using it: capture a block over the serial
 * port and look at it offline. This is how the RP2040 question above gets
 * answered with data instead of an assumption. Never use it as a random
 * number generator in application code - that is what the DRBG is for.
 */
int wiz_entropy_raw(unsigned char *out, size_t len);

/** A short name for the active source, for logs and banners. */
const char *wiz_entropy_source_name(void);

#ifdef __cplusplus
}
#endif

#endif /* WIZ_ENTROPY_H */
