/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PltrClientEnrollment PltrClientEnrollment;
// The target key is vouched for by authenticated Setup, not yet a saved pin.
// Success proves possession of that key AND durable acceptance of this Client.
PltrClientEnrollment *pltr_client_enrollment_create(
    const uint8_t private_key[32], const uint8_t target_key[32], const uint8_t request_id[16]);
void pltr_client_enrollment_destroy(PltrClientEnrollment *client);
int pltr_client_enrollment_start(PltrClientEnrollment *client,
    uint8_t *out, size_t capacity, size_t *written);
// 0 incomplete, 1 send confirmation, 2 enrollment committed, -1 refuse/close.
// No pin or persistent Client state is ever written by this codec.
int pltr_client_enrollment_receive(PltrClientEnrollment *client,
    const uint8_t *bytes, size_t size, size_t *consumed,
    uint8_t *reply, size_t capacity, size_t *written);
#ifdef __cplusplus
}
#endif
