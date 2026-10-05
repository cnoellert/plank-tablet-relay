/* Portable public drawing-status serializer and literal validator.
 *
 * Contract: docs/relay-drawing-handoff-contract.md, revision
 * plank-drawing-handoff-v1+r3, entry point 1 (local listener metadata, section
 * 3) with the deterministic literal pre-checks of section 7.4a and the
 * boundAddress order of section 7.5.
 *
 * This translation unit is deliberately platform-portable: libc only, no
 * AF_UNIX, no libsodium, no libudev, so its tests compile and run on macOS
 * (contract section 12.1). Socket plumbing lives in drawing_status_server.hpp.
 *
 * Nothing here reads, derives from or reports a private key, the paired-client
 * allowlist, tablet samples, capture-lease state, file paths or process
 * identifiers (contract section 8.4). The caller passes in the 32-byte public
 * key and the address the listener is actually bound to; this unit only formats.
 */
#ifndef PLTR_DRAWING_STATUS_H
#define PLTR_DRAWING_STATUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Abstract socket name, contract section 8.2. MUST differ from the capture
 * interlock name plank-tablet-capture-v1 (src/capture_lease.hpp:9). */
#define PLTR_DRAWING_STATUS_NAME "plank-tablet-drawing-status-v1"

/* Production accepts exactly one peer uid, contract section 8.3. There is no
 * configuration surface that can widen this: no environment variable, no
 * configuration key, no command-line flag, no shipped compile-time define. */
#define PLTR_DRAWING_STATUS_ACCEPTED_UID 0u

/* Bounds, contract section 8.2, including the terminating newline. */
#define PLTR_DRAWING_STATUS_REQUEST_MAX 256
#define PLTR_DRAWING_STATUS_RESPONSE_MAX 4096
#define PLTR_DRAWING_STATUS_CONNECTION_MS 500
#define PLTR_DRAWING_STATUS_BACKLOG 4

typedef enum {
    PLTR_BOUND_ADDRESS_OK = 0,
    PLTR_BOUND_ADDRESS_MISSING,
    PLTR_BOUND_ADDRESS_TOO_LONG,
    PLTR_BOUND_ADDRESS_NOT_LITERAL,
    PLTR_BOUND_ADDRESS_NON_CANONICAL,
    PLTR_BOUND_ADDRESS_MAPPED,
    PLTR_BOUND_ADDRESS_FAMILY_UNSUPPORTED,
    PLTR_BOUND_ADDRESS_INVALID
} PltrBoundAddressResult;

/* Section 7.5 ordered checks over the raw text. NULL is MISSING. */
PltrBoundAddressResult pltr_drawing_status_check_bound_address(const char *text);

/* The contract reason identifier for a result, or NULL for OK. */
const char *pltr_drawing_status_bound_address_reason(PltrBoundAddressResult result);

/* Section 8.3 server-side peer policy. Returns 1 when the peer is accepted. */
int pltr_drawing_status_peer_accepted(uint32_t peer_uid, uint32_t accepted_uid);

/* 64 lowercase hex characters plus a terminator, contract section 5.1. */
void pltr_drawing_status_identity_hex(const uint8_t public_key[32], char out[65]);

/* Serialize the newline-terminated responses of contract section 8.4. Each
 * returns the number of bytes written, or -1 when the arguments or the capacity
 * are unusable. Output is compact JSON followed by exactly one '\n'. */
int pltr_drawing_status_ready(const uint8_t public_key[32],
                              const char *bound_address, uint16_t bound_port,
                              char *out, size_t capacity);
int pltr_drawing_status_unavailable(const char *reason, char *out, size_t capacity);
int pltr_drawing_status_error(const char *text, char *out, size_t capacity);

/* The only accepted request is {"op":"drawing-status","version":1}: exactly two
 * members, no others, no nesting. Returns 1 when accepted. */
int pltr_drawing_status_check_request(const char *bytes, size_t length);
/* V2 adds the independently bound Bluetooth drawing endpoint capability. */
int pltr_drawing_status_request_version(const char *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
