#include "noise.h"
#include "protocol.h"

#include <sodium.h>
#include <limits.h>
#include <string.h>

static const uint8_t protocol_name[] = "Noise_IK_25519_ChaChaPoly_BLAKE2b";
static const uint8_t relay_prologue[] = "PLANK-TABLET-RELAY/1";
enum { INIT_WRITE = 1, INIT_READ = 2, RESP_READ = 3,
       RESP_WRITE = 4, TRANSPORT = 5, FAILED = 6 };

static int fail(PltrNoise *s) {
    sodium_memzero(s, sizeof(*s));
    s->stage = FAILED;
    return -1;
}

static void hmac64(uint8_t out[64], const uint8_t key[64],
                   const uint8_t *message, size_t size) {
    uint8_t ipad[128], opad[128], inner[64];
    memset(ipad, 0x36, sizeof(ipad));
    memset(opad, 0x5c, sizeof(opad));
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] ^= key[i];
        opad[i] ^= key[i];
    }
    crypto_generichash_state h;
    crypto_generichash_init(&h, NULL, 0, 64);
    crypto_generichash_update(&h, ipad, sizeof(ipad));
    if (size) crypto_generichash_update(&h, message, size);
    crypto_generichash_final(&h, inner, sizeof(inner));
    crypto_generichash_init(&h, NULL, 0, 64);
    crypto_generichash_update(&h, opad, sizeof(opad));
    crypto_generichash_update(&h, inner, sizeof(inner));
    crypto_generichash_final(&h, out, 64);
    sodium_memzero(&h, sizeof(h));
    sodium_memzero(inner, sizeof(inner));
    sodium_memzero(ipad, sizeof(ipad));
    sodium_memzero(opad, sizeof(opad));
}

static void hkdf2(const uint8_t key[64], const uint8_t *input, size_t size,
                  uint8_t first[64], uint8_t second[64]) {
    uint8_t temporary[64], message[65];
    hmac64(temporary, key, input, size);
    const uint8_t one = 1;
    hmac64(first, temporary, &one, 1);
    memcpy(message, first, 64);
    message[64] = 2;
    hmac64(second, temporary, message, sizeof(message));
    sodium_memzero(temporary, sizeof(temporary));
    sodium_memzero(message, sizeof(message));
}

static void mix_hash(PltrNoise *s, const uint8_t *bytes, size_t size) {
    uint8_t result[64];
    crypto_generichash_state h;
    crypto_generichash_init(&h, NULL, 0, 64);
    crypto_generichash_update(&h, s->handshake_hash, 64);
    if (size) crypto_generichash_update(&h, bytes, size);
    crypto_generichash_final(&h, result, 64);
    memcpy(s->handshake_hash, result, 64);
    sodium_memzero(&h, sizeof(h));
    sodium_memzero(result, sizeof(result));
}

static void mix_key(PltrNoise *s, const uint8_t input[32]) {
    uint8_t ck[64], temp[64];
    hkdf2(s->chaining_key, input, 32, ck, temp);
    memcpy(s->chaining_key, ck, 64);
    memcpy(s->handshake_key, temp, 32);
    s->has_handshake_key = 1;
    s->handshake_nonce = 0;
    sodium_memzero(ck, sizeof(ck));
    sodium_memzero(temp, sizeof(temp));
}

static void nonce_bytes(uint8_t nonce[12], uint64_t counter) {
    memset(nonce, 0, 4);
    for (unsigned i = 0; i < 8; ++i) nonce[4 + i] = (uint8_t)(counter >> (i * 8));
}

static int aead_encrypt(const uint8_t key[32], uint64_t *counter,
                        const uint8_t *ad, size_t ad_size,
                        const uint8_t *plain, size_t size, uint8_t *out) {
    if (*counter == UINT64_MAX) return -1;
    uint8_t nonce[12];
    unsigned long long written = 0;
    nonce_bytes(nonce, *counter);
    int result = crypto_aead_chacha20poly1305_ietf_encrypt(
        out, &written, plain, size, ad, ad_size, NULL, nonce, key);
    sodium_memzero(nonce, sizeof(nonce));
    if (result != 0 || written != size + PLTR_NOISE_TAG_SIZE) return -1;
    ++*counter;
    return 0;
}

static int aead_decrypt(const uint8_t key[32], uint64_t *counter,
                        const uint8_t *ad, size_t ad_size,
                        const uint8_t *cipher, size_t size, uint8_t *out) {
    if (*counter == UINT64_MAX || size < PLTR_NOISE_TAG_SIZE) return -1;
    uint8_t nonce[12];
    unsigned long long written = 0;
    nonce_bytes(nonce, *counter);
    int result = crypto_aead_chacha20poly1305_ietf_decrypt(
        out, &written, NULL, cipher, size, ad, ad_size, nonce, key);
    sodium_memzero(nonce, sizeof(nonce));
    if (result != 0 || written != size - PLTR_NOISE_TAG_SIZE) return -1;
    ++*counter;
    return 0;
}

static int encrypt_hash(PltrNoise *s, const uint8_t *plain, size_t size,
                        uint8_t *out) {
    if (!s->has_handshake_key) {
        if (size) memcpy(out, plain, size);
        mix_hash(s, out, size);
        return 0;
    }
    if (aead_encrypt(s->handshake_key, &s->handshake_nonce,
                     s->handshake_hash, 64, plain, size, out) != 0) return -1;
    mix_hash(s, out, size + PLTR_NOISE_TAG_SIZE);
    return 0;
}

static int decrypt_hash(PltrNoise *s, const uint8_t *cipher, size_t size,
                        uint8_t *out) {
    if (!s->has_handshake_key) return -1;
    if (aead_decrypt(s->handshake_key, &s->handshake_nonce,
                     s->handshake_hash, 64, cipher, size, out) != 0) return -1;
    mix_hash(s, cipher, size);
    return 0;
}

static int dh_mix(PltrNoise *s, const uint8_t private_key[32],
                  const uint8_t public_key[32]) {
    uint8_t result[32];
    if (crypto_scalarmult_curve25519(result, private_key, public_key) != 0) return -1;
    mix_key(s, result);
    sodium_memzero(result, sizeof(result));
    return 0;
}

int pltr_noise_public_key(const uint8_t private_key[32], uint8_t public_key[32]) {
    if (private_key == NULL || public_key == NULL || sodium_init() < 0) return -1;
    return crypto_scalarmult_curve25519_base(public_key, private_key);
}

static int init_with_prologue(PltrNoise *s, PltrNoiseRole role,
                              const uint8_t private_key[32],
                              const uint8_t remote_static_key[32],
                              const uint8_t *prologue, size_t prologue_size) {
    if (s == NULL || private_key == NULL ||
        (prologue == NULL && prologue_size != 0) ||
        (role == PLTR_NOISE_INITIATOR && remote_static_key == NULL) ||
        (role != PLTR_NOISE_INITIATOR && role != PLTR_NOISE_RESPONDER) ||
        sodium_init() < 0) return -1;
    memset(s, 0, sizeof(*s));
    memcpy(s->static_private, private_key, 32);
    if (pltr_noise_public_key(private_key, s->static_public) != 0) {
        pltr_noise_clear(s); return -1;
    }
    if (remote_static_key) memcpy(s->remote_static, remote_static_key, 32);
    memset(s->handshake_hash, 0, 64);
    memcpy(s->handshake_hash, protocol_name, sizeof(protocol_name) - 1);
    memcpy(s->chaining_key, s->handshake_hash, 64);
    mix_hash(s, prologue, prologue_size);
    if (role == PLTR_NOISE_INITIATOR) {
        mix_hash(s, remote_static_key, 32);
        s->stage = INIT_WRITE;
    } else {
        mix_hash(s, s->static_public, 32);
        s->stage = RESP_READ;
    }
    return 0;
}

int pltr_noise_init(PltrNoise *s, PltrNoiseRole role,
                    const uint8_t private_key[32],
                    const uint8_t remote_static_key[32], uint8_t link_type) {
    if (link_type != 1 && link_type != 2) {
        if (s) pltr_noise_clear(s);
        return -1;
    }
    uint8_t prologue[sizeof(relay_prologue)];
    memcpy(prologue, relay_prologue, sizeof(relay_prologue) - 1);
    prologue[sizeof(relay_prologue) - 1] = link_type;
    return init_with_prologue(s, role, private_key, remote_static_key,
                              prologue, sizeof(prologue));
}

int pltr_noise_init_enrollment(PltrNoise *s, PltrNoiseRole role,
    const uint8_t private_key[32], const uint8_t relay_public_key[32]) {
    static const uint8_t prologue[] = "PLANK-DRAWING-ENROLLMENT/1";
    return init_with_prologue(s, role, private_key, relay_public_key,
                              prologue, sizeof(prologue) - 1);
}

#ifdef PLTR_NOISE_TESTING
int pltr_noise_init_test(PltrNoise *s, PltrNoiseRole role,
                         const uint8_t private_key[32],
                         const uint8_t remote_static_key[32],
                         const uint8_t *prologue, size_t prologue_size) {
    return init_with_prologue(s, role, private_key, remote_static_key,
                              prologue, prologue_size);
}
void pltr_noise_set_ephemeral_test(PltrNoise *s, const uint8_t private_key[32]) {
    memcpy(s->test_ephemeral, private_key, 32);
    s->has_test_ephemeral = 1;
}
#endif

void pltr_noise_clear(PltrNoise *s) {
    if (s) sodium_memzero(s, sizeof(*s));
}

static int new_ephemeral(PltrNoise *s) {
#ifdef PLTR_NOISE_TESTING
    if (s->has_test_ephemeral) {
        memcpy(s->ephemeral_private, s->test_ephemeral, 32);
        sodium_memzero(s->test_ephemeral, 32);
        s->has_test_ephemeral = 0;
    } else
#endif
    randombytes_buf(s->ephemeral_private, 32);
    return pltr_noise_public_key(s->ephemeral_private, s->ephemeral_public);
}

static void split(PltrNoise *s, int initiator) {
    uint8_t first[64], second[64];
    hkdf2(s->chaining_key, NULL, 0, first, second);
    memcpy(s->send_key, initiator ? first : second, 32);
    memcpy(s->receive_key, initiator ? second : first, 32);
    s->send_nonce = s->receive_nonce = 0;
    sodium_memzero(first, sizeof(first));
    sodium_memzero(second, sizeof(second));
    sodium_memzero(s->static_private, 32);
    sodium_memzero(s->ephemeral_private, 32);
    sodium_memzero(s->handshake_key, 32);
    sodium_memzero(s->chaining_key, 64);
    s->stage = TRANSPORT;
}

int pltr_noise_write_first(PltrNoise *s, const uint8_t *payload, size_t size,
                           uint8_t *out, size_t capacity, size_t *written) {
    if (s == NULL || s->stage != INIT_WRITE || out == NULL || written == NULL ||
        (payload == NULL && size != 0) || size > PLTR_MAX_PAYLOAD_SIZE - 96 ||
        capacity < 96 + size || new_ephemeral(s) != 0) return -1;
    memcpy(out, s->ephemeral_public, 32);
    mix_hash(s, out, 32);
    if (dh_mix(s, s->ephemeral_private, s->remote_static) != 0 ||
        encrypt_hash(s, s->static_public, 32, out + 32) != 0 ||
        dh_mix(s, s->static_private, s->remote_static) != 0 ||
        encrypt_hash(s, payload, size, out + 80) != 0) {
        return fail(s);
    }
    *written = 96 + size;
    s->stage = INIT_READ;
    return 0;
}

int pltr_noise_read_first(PltrNoise *s, const uint8_t *message, size_t size,
                          uint8_t client_static_key[32], uint8_t *payload,
                          size_t capacity, size_t *read) {
    if (s == NULL || s->stage != RESP_READ) return -1;
    if (message == NULL || client_static_key == NULL || payload == NULL ||
        read == NULL || size < 96 || size > PLTR_MAX_PAYLOAD_SIZE ||
        capacity < size - 96) return fail(s);
    memcpy(s->remote_ephemeral, message, 32);
    mix_hash(s, message, 32);
    if (dh_mix(s, s->static_private, s->remote_ephemeral) != 0 ||
        decrypt_hash(s, message + 32, 48, s->remote_static) != 0 ||
        dh_mix(s, s->static_private, s->remote_static) != 0 ||
        decrypt_hash(s, message + 80, size - 80, payload) != 0) {
        sodium_memzero(payload, size - 96);
        return fail(s);
    }
    memcpy(client_static_key, s->remote_static, 32);
    *read = size - 96;
    s->stage = RESP_WRITE;
    return 0;
}

int pltr_noise_write_second(PltrNoise *s,
                            const uint8_t approved_client_key[32],
                            const uint8_t *payload, size_t size,
                            uint8_t *out, size_t capacity, size_t *written) {
    if (s == NULL || s->stage != RESP_WRITE || approved_client_key == NULL) return -1;
    if (sodium_memcmp(approved_client_key, s->remote_static, 32) != 0)
        return fail(s);
    if (out == NULL || written == NULL || (payload == NULL && size != 0) ||
        size > PLTR_MAX_PAYLOAD_SIZE - 48 || capacity < 48 + size ||
        new_ephemeral(s) != 0) return -1;
    memcpy(out, s->ephemeral_public, 32);
    mix_hash(s, out, 32);
    if (dh_mix(s, s->ephemeral_private, s->remote_ephemeral) != 0 ||
        dh_mix(s, s->ephemeral_private, s->remote_static) != 0 ||
        encrypt_hash(s, payload, size, out + 32) != 0) {
        return fail(s);
    }
    *written = 48 + size;
    split(s, 0);
    return 0;
}

int pltr_noise_read_second(PltrNoise *s, const uint8_t *message, size_t size,
                           uint8_t *payload, size_t capacity, size_t *read) {
    if (s == NULL || s->stage != INIT_READ) return -1;
    if (message == NULL || payload == NULL || read == NULL || size < 48 ||
        size > PLTR_MAX_PAYLOAD_SIZE || capacity < size - 48) return fail(s);
    memcpy(s->remote_ephemeral, message, 32);
    mix_hash(s, message, 32);
    if (dh_mix(s, s->ephemeral_private, s->remote_ephemeral) != 0 ||
        dh_mix(s, s->static_private, s->remote_ephemeral) != 0 ||
        decrypt_hash(s, message + 32, size - 32, payload) != 0) {
        sodium_memzero(payload, size - 48);
        return fail(s);
    }
    *read = size - 48;
    split(s, 1);
    return 0;
}

int pltr_noise_encrypt(PltrNoise *s, const uint8_t *plain, size_t size,
                       uint8_t *out, size_t capacity, size_t *written) {
    if (s == NULL || s->stage != TRANSPORT || plain == NULL || out == NULL ||
        written == NULL || size > PLTR_MAX_FRAME_SIZE || capacity < size + 16)
        return -1;
    if (aead_encrypt(s->send_key, &s->send_nonce, NULL, 0, plain, size, out) != 0) {
        return fail(s);
    }
    *written = size + 16;
    return 0;
}

int pltr_noise_decrypt(PltrNoise *s, const uint8_t *cipher, size_t size,
                       uint8_t *out, size_t capacity, size_t *read) {
    if (s == NULL || s->stage != TRANSPORT) return -1;
    if (cipher == NULL || out == NULL || read == NULL || size < 16 ||
        size > PLTR_MAX_RECORD_BODY_SIZE || capacity < size - 16) return fail(s);
    if (aead_decrypt(s->receive_key, &s->receive_nonce,
                     NULL, 0, cipher, size, out) != 0) {
        sodium_memzero(out, size - 16);
        return fail(s);
    }
    *read = size - 16;
    return 0;
}
