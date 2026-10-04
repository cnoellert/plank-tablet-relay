/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "client_enrollment.h"
#include "noise.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
struct PltrClientEnrollment {
    PltrNoise noise;
    uint8_t id[16], bytes[64];
    size_t received, expected;
    int stage;
};
static int fail(PltrClientEnrollment *c) {
    pltr_noise_clear(&c->noise); c->stage = -1; return -1;
}
PltrClientEnrollment *pltr_client_enrollment_create(const uint8_t private_key[32],
    const uint8_t target_key[32], const uint8_t request_id[16]) {
    if (!private_key || !target_key || !request_id) return NULL;
    PltrClientEnrollment *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (pltr_noise_init_enrollment(&c->noise, PLTR_NOISE_INITIATOR,
                                  private_key, target_key) != 0) {
        free(c); return NULL;
    }
    memcpy(c->id, request_id, 16); return c;
}
void pltr_client_enrollment_destroy(PltrClientEnrollment *c) {
    if (c) { sodium_memzero(c, sizeof(*c)); free(c); }
}
int pltr_client_enrollment_start(PltrClientEnrollment *c, uint8_t *out,
    size_t capacity, size_t *written) {
    size_t n;
    if (!c || c->stage || !out || !written || capacity < 119) return -1;
    memcpy(out, "PLEN\1", 5);
    if (pltr_noise_write_first(&c->noise, c->id, 16, out + 7,
                               capacity - 7, &n) != 0 || n != 112) return fail(c);
    out[5] = (uint8_t)n; out[6] = 0;
    *written = n + 7; c->stage = 1; c->expected = 50; return 0;
}
int pltr_client_enrollment_receive(PltrClientEnrollment *c, const uint8_t *bytes,
    size_t size, size_t *consumed, uint8_t *reply, size_t capacity, size_t *written) {
    if (!c || !bytes || !consumed || !reply || !written ||
        (c->stage != 1 && c->stage != 2)) return -1;
    *written = 0;
    *consumed = size < c->expected - c->received ? size : c->expected - c->received;
    memcpy(c->bytes + c->received, bytes, *consumed); c->received += *consumed;
    if (c->received >= 2 && (c->bytes[0] != c->expected - 2 || c->bytes[1])) return fail(c);
    if (c->received != c->expected) return 0;
    uint8_t plain[21]; size_t n;
    if (c->stage == 1) {
        if (pltr_noise_read_second(&c->noise, c->bytes + 2, 48, plain,
                                   sizeof(plain), &n) != 0 || n != 0 || capacity < 39) return fail(c);
        memcpy(plain, "PLEN\1", 5); memcpy(plain + 5, c->id, 16);
        if (pltr_noise_encrypt(&c->noise, plain, 21, reply + 2, capacity - 2, &n) != 0 || n != 37) return fail(c);
        reply[0] = 37; reply[1] = 0; *written = 39;
        c->stage = 2; c->received = 0; c->expected = 39; return 1;
    }
    if (pltr_noise_decrypt(&c->noise, c->bytes + 2, 37, plain, sizeof(plain), &n) != 0 ||
        n != 21 || memcmp(plain, "PLEN\1", 5) || sodium_memcmp(plain + 5, c->id, 16)) return fail(c);
    c->stage = 3; pltr_noise_clear(&c->noise); return 2;
}
