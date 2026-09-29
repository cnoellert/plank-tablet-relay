// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PLANK_BLE_LAB_H
#define PLANK_BLE_LAB_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PltrBleLab PltrBleLab;
PltrBleLab *pltr_ble_lab_create(const char *directory);
void pltr_ble_lab_destroy(PltrBleLab *lab);
int pltr_ble_lab_open_pairing(PltrBleLab *lab, uint64_t wall_seconds, uint64_t now_ms);
void pltr_ble_lab_disconnect(PltrBleLab *lab, uint64_t now_ms);
int pltr_ble_lab_receive(PltrBleLab *lab, const uint8_t *data, size_t size,
    size_t *consumed, uint64_t now_ms, uint8_t *out, size_t capacity, size_t *written);
int pltr_ble_lab_key(PltrBleLab *lab, uint8_t key, uint64_t now_ms,
    uint8_t *out, size_t capacity, size_t *written);
void pltr_ble_lab_tablet(PltrBleLab *lab, int attached);
int pltr_ble_lab_button(PltrBleLab *lab, uint16_t code, int value,
    uint64_t wall_seconds, uint64_t now_ms,
    uint8_t *out, size_t capacity, size_t *written);
int pltr_ble_lab_tick(PltrBleLab *lab, uint64_t now_ms,
    uint8_t *out, size_t capacity, size_t *written);
int pltr_ble_lab_observing(const PltrBleLab *lab);
// Local diagnostic state only: 0 = not waiting, 1 = new Client, 2 = known Client.
int pltr_ble_lab_approval_pending(const PltrBleLab *lab);
int pltr_ble_lab_sample(PltrBleLab *lab, const uint8_t *payload, size_t size,
    uint8_t *out, size_t capacity, size_t *written);
// Serve one accepted Linux LE Credit Based L2CAP channel with the same saved
// identity and raw-HID worker as TCP. Pairing must be idle for its lifetime.
// The caller owns coc_fd and wakes stop_fd to interrupt the session.
int pltr_ble_lab_run_coc_session(PltrBleLab *lab, int coc_fd, int stop_fd);
#ifdef __cplusplus
}
#endif
#endif
