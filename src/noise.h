#ifndef PLANK_RELAY_NOISE_H
#define PLANK_RELAY_NOISE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLTR_NOISE_KEY_SIZE 32u
#define PLTR_NOISE_HASH_SIZE 64u
#define PLTR_NOISE_TAG_SIZE 16u

typedef enum PltrNoiseRole {
    PLTR_NOISE_INITIATOR = 1,
    PLTR_NOISE_RESPONDER = 2,
} PltrNoiseRole;

typedef struct PltrNoise {
    uint8_t static_private[32], static_public[32], remote_static[32];
    uint8_t ephemeral_private[32], ephemeral_public[32], remote_ephemeral[32];
    uint8_t chaining_key[64], handshake_hash[64], handshake_key[32];
    uint8_t send_key[32], receive_key[32];
    uint64_t handshake_nonce, send_nonce, receive_nonce;
    unsigned stage, has_handshake_key;
#ifdef PLTR_NOISE_TESTING
    uint8_t test_ephemeral[32];
    unsigned has_test_ephemeral;
#endif
} PltrNoise;

// link_type is 1 for Bluetooth LE or 2 for TCP. The production prologue is
// fixed here so a caller cannot accidentally establish a cross-link session.
int pltr_noise_init(PltrNoise *state, PltrNoiseRole role,
                    const uint8_t private_key[32],
                    const uint8_t remote_static_key[32], uint8_t link_type);
void pltr_noise_clear(PltrNoise *state);
// Separate domain for a Setup-authorized enrollment proof. It cannot be
// replayed as a drawing handshake; no caller-selected prologue is exposed.
int pltr_noise_init_enrollment(PltrNoise *state, PltrNoiseRole role,
    const uint8_t private_key[32], const uint8_t relay_public_key[32]);
int pltr_noise_public_key(const uint8_t private_key[32], uint8_t public_key[32]);
int pltr_noise_write_first(PltrNoise *state, const uint8_t *payload, size_t size,
                           uint8_t *out, size_t capacity, size_t *written);
int pltr_noise_read_first(PltrNoise *state, const uint8_t *message, size_t size,
                          uint8_t client_static_key[32], uint8_t *payload,
                          size_t capacity, size_t *read);
// approved_client_key comes from persistent pairing or a claimed, authenticated
// Setup enrollment grant. It must equal the decrypted identity from message one.
int pltr_noise_write_second(PltrNoise *state,
                            const uint8_t approved_client_key[32],
                            const uint8_t *payload, size_t size,
                            uint8_t *out, size_t capacity, size_t *written);
int pltr_noise_read_second(PltrNoise *state, const uint8_t *message, size_t size,
                           uint8_t *payload, size_t capacity, size_t *read);
int pltr_noise_encrypt(PltrNoise *state, const uint8_t *plaintext, size_t size,
                       uint8_t *out, size_t capacity, size_t *written);
int pltr_noise_decrypt(PltrNoise *state, const uint8_t *ciphertext, size_t size,
                       uint8_t *out, size_t capacity, size_t *read);

#ifdef PLTR_NOISE_TESTING
int pltr_noise_init_test(PltrNoise *state, PltrNoiseRole role,
                         const uint8_t private_key[32],
                         const uint8_t remote_static_key[32],
                         const uint8_t *prologue, size_t prologue_size);
void pltr_noise_set_ephemeral_test(PltrNoise *state, const uint8_t private_key[32]);
#endif

#ifdef __cplusplus
}
#endif

#endif
