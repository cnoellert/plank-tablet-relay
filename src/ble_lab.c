// SPDX-License-Identifier: GPL-3.0-or-later
// Small opaque adapter for the BlueZ diagnostic service. Cryptographic and
// persistent pairing behavior come from the production protocol implementation.
#include "ble_lab.h"
#include "ble_lab_internal.h"
#include "identity.h"
#include "link.h"
#include "pair_budget.h"
#include "pair_wire.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

struct PltrBleLab {
    PltrIdentityStore store;
    PltrPairing pairing;
    PltrPairWire wire;
    PltrLink link;
    PltrRecordReader probe;
    unsigned mode;
    uint64_t started_ms, received_ms, ping_ms;
    uint64_t status_ms, pressed_ms, released_ms;
    uint16_t button, held;
    unsigned presses, prior_failures;
    int attached, status_dirty, physically_approved;
};

PltrIdentityStore *pltr_ble_lab_identity_store(PltrBleLab *lab) {
    return lab ? &lab->store : NULL;
}

int pltr_ble_lab_pairing_busy(const PltrBleLab *lab) {
    return lab && (lab->mode == 2 || lab->mode == 3) &&
        lab->wire.stage != PLTR_PAIR_WIRE_DONE;
}

static void clear_presses(PltrBleLab *lab) {
    lab->button = lab->held = 0;
    lab->presses = 0;
    lab->pressed_ms = lab->released_ms = 0;
    lab->status_dirty = 1;
}

void pltr_ble_lab_tablet(PltrBleLab *lab, int attached) {
    if (!lab) return;
    if (lab->attached != !!attached) clear_presses(lab);
    lab->attached = !!attached;
}

static int approval_status(PltrBleLab *lab, uint64_t now_ms,
    uint8_t *out, size_t capacity, size_t *written) {
    uint64_t remaining = lab->pairing.deadline_ms > now_ms ?
        (lab->pairing.deadline_ms - now_ms + 999) / 1000 : 0;
    uint8_t status[8] = {1, (uint8_t)lab->attached, (uint8_t)lab->presses, 3,
        (uint8_t)remaining, 0, (uint8_t)lab->button, (uint8_t)(lab->button >> 8)};
    lab->status_ms = now_ms;
    lab->status_dirty = 0;
    return pltr_pair_wire_approval_status(&lab->wire, status, out, capacity, written);
}

PltrBleLab *pltr_ble_lab_create(const char *directory) {
    if (!directory || sodium_init() < 0) return NULL;
    PltrBleLab *lab = calloc(1, sizeof(*lab));
    if (!lab) return NULL;
    lab->store.directory_fd = lab->store.lock_fd = -1;
    const uint8_t name[] = "PLANK Relay Lab";
    if (pltr_identity_store_open(&lab->store, directory) != 0 ||
        pltr_pairing_init(&lab->pairing, &lab->store, name, sizeof(name)-1) != 0) {
        pltr_ble_lab_destroy(lab);
        return NULL;
    }
    pltr_record_reader_init(&lab->probe, PLTR_PRE_AUTH);
    return lab;
}

void pltr_ble_lab_destroy(PltrBleLab *lab) {
    if (!lab) return;
    pltr_link_clear(&lab->link);
    pltr_pairing_clear(&lab->pairing);
    pltr_identity_store_close(&lab->store);
    sodium_memzero(lab, sizeof(*lab));
    free(lab);
}

int pltr_ble_lab_open_pairing(PltrBleLab *lab, uint64_t wall_seconds, uint64_t now_ms) {
    if (!lab || lab->mode || lab->probe.filled ||
        pltr_pair_budget_reserve(&lab->store, wall_seconds) != 0) return -1;
    return pltr_pairing_open(&lab->pairing, now_ms, 0);
}

void pltr_ble_lab_disconnect(PltrBleLab *lab, uint64_t now_ms) {
    if (!lab) return;
    if (lab->mode == 2 || lab->mode == 3) pltr_pair_wire_close(&lab->wire, now_ms);
    // Unapproved remote requests cannot consume the physical-attempt budget.
    if (lab->mode == 3 && !lab->physically_approved) {
        lab->pairing.stage = PLTR_PAIR_CLOSED;
        lab->pairing.failures = lab->prior_failures;
        lab->pairing.lockout_until_ms = 0;
    }
    pltr_link_clear(&lab->link);
    memset(&lab->wire, 0, sizeof(lab->wire));
    pltr_record_reader_init(&lab->probe, PLTR_PRE_AUTH);
    lab->mode = 0;
    lab->started_ms = lab->received_ms = lab->ping_ms = 0;
    lab->physically_approved = 0;
    lab->status_ms = 0;
    clear_presses(lab);
}

int pltr_ble_lab_receive(PltrBleLab *lab, const uint8_t *data, size_t size,
    size_t *consumed, uint64_t now_ms, uint8_t *out, size_t capacity, size_t *written) {
    if (!lab || !data || !size || !consumed || !out || !written) return -1;
    *consumed = *written = 0;
    if (!lab->started_ms) lab->started_ms = now_ms;
    lab->received_ms = now_ms;
    if (!lab->mode) {
        const uint8_t *body;
        size_t body_size;
        int result = pltr_record_reader_push(&lab->probe, data, size,
                                             consumed, &body, &body_size);
        if (result <= 0) return result;
        PltrFrame first;
        if (pltr_decode_frame(body, body_size, PLTR_CLIENT_TO_RELAY,
                              PLTR_PRE_AUTH, 1, &first) != 0 ||
            first.type != PLTR_OPEN) return -1;
        lab->mode = first.payload[0];
        if (lab->mode == 2 || lab->mode == 3) {
            if (lab->mode == 3) {
                lab->prior_failures = lab->pairing.failures;
                if (pltr_pairing_open(&lab->pairing, now_ms, 0) != 0) {
                    lab->mode = 0; // Preserve a pre-existing lockout on failure.
                    return -1;
                }
                clear_presses(lab);
            }
            if (pltr_pair_wire_init(&lab->wire, &lab->pairing, 1) != 0) return -1;
            if (lab->mode == 3 && pltr_pair_wire_button_approval(&lab->wire) != 0) return -1;
        } else {
            if (pltr_link_init(&lab->link, PLTR_NOISE_RESPONDER,
                lab->store.private_key, NULL, pltr_identity_store_approve,
                &lab->store, 1) != 0 ||
                pltr_link_enable_input_observer(&lab->link) != 0) return -1;
        }
        // Replay the already validated OPEN into the selected stream parser.
        size_t replayed = 0;
        return pltr_ble_lab_receive(lab, lab->probe.bytes, body_size + 2,
            &replayed, now_ms, out, capacity, written);
    }
    if (lab->mode == 2 || lab->mode == 3) {
        int result = pltr_pair_wire_receive(&lab->wire, data, size, consumed,
                                            now_ms, out, capacity, written);
        if (result >= 0 && lab->wire.stage == PLTR_PAIR_WIRE_DONE &&
            *written == 2 + PLTR_HEADER_SIZE + 33 && out[2 + PLTR_HEADER_SIZE] == 0 &&
            pltr_pair_budget_succeeded(&lab->store) != 0) return -1;
        if (result == 1 && lab->mode == 3 && lab->wire.stage == PLTR_PAIR_WIRE_KEYS)
            return approval_status(lab, now_ms, out, capacity, written);
        return result;
    }
    PltrFrame frame;
    int result = pltr_link_receive(&lab->link, data, size, consumed,
                                    out, capacity, written, &frame);
    if (result != 1 || !frame.type) return result;
    if (frame.type == PLTR_PONG || frame.type == PLTR_INPUT_OBSERVE ||
        frame.type == PLTR_GOODBYE) return 1;
    if (frame.type == PLTR_PING) {
        uint8_t pong[32] = {0};
        memcpy(pong, frame.payload, 16);
        for (unsigned i = 0; i < 8; ++i) {
            pong[16+i] = pong[24+i] = (uint8_t)((now_ms * 1000) >> (8*i));
        }
        return pltr_link_send(&lab->link, PLTR_PONG, pong, sizeof(pong),
                               out, capacity, written) == 0 ? 1 : -1;
    }
    // This observer never negotiates Host features or starts a raw-HID worker.
    return -1;
}

int pltr_ble_lab_key(PltrBleLab *lab, uint8_t key, uint64_t now_ms,
    uint8_t *out, size_t capacity, size_t *written) {
    if (!lab || !written) return -1;
    *written = 0;
    if (lab->mode != 2 || lab->wire.stage != PLTR_PAIR_WIRE_KEYS) return 0;
    return pltr_pair_wire_key(&lab->wire, key, now_ms, out, capacity, written);
}

int pltr_ble_lab_button(PltrBleLab *lab, uint16_t code, int value,
    uint64_t wall_seconds, uint64_t now_ms,
    uint8_t *out, size_t capacity, size_t *written) {
    if (!lab || !out || !written || code == 0 || code >= 768) return -1;
    *written = 0;
    if (lab->mode != 3 || lab->wire.stage != PLTR_PAIR_WIRE_KEYS || !lab->attached) return 0;
    if (pltr_pairing_tick(&lab->pairing, now_ms) != 0 ||
        lab->pairing.stage != PLTR_PAIR_WAIT_CODE) return -1;
    if (value == 2) return 0; // autorepeat is never a press
    if (value != 0 && value != 1) return -1;
    if (value == 1) {
        if (lab->held) { clear_presses(lab); return 0; }
        if (lab->button != code || (lab->presses && now_ms - lab->released_ms > 2000))
            clear_presses(lab);
        lab->held = lab->button = code;
        lab->pressed_ms = now_ms;
        return 0;
    }
    if (lab->held != code) return 0; // release of a key held before this request
    lab->held = 0;
    if (now_ms < lab->pressed_ms || now_ms - lab->pressed_ms < 30 ||
        now_ms - lab->pressed_ms > 1000) { clear_presses(lab); return 0; }
    lab->released_ms = now_ms;
    lab->status_dirty = 1;
    if (++lab->presses < 3) return approval_status(lab, now_ms, out, capacity, written);
    lab->physically_approved = 1;
    if (pltr_pair_budget_reserve(&lab->store, wall_seconds) != 0) return -1;
    // The gesture is a local gate. This public value only reuses the existing
    // ephemeral exchange/confirmation; it does NOT authenticate first pairing.
    int result = 0;
    for (unsigned i = 0; i < 5; ++i) {
        result = pltr_pair_wire_key(&lab->wire, PLTR_BUTTON_APPROVAL_CODE[i] - '0',
            now_ms, out, capacity, written);
        if (result < 0) return result;
    }
    return result;
}

int pltr_ble_lab_tick(PltrBleLab *lab, uint64_t now_ms,
    uint8_t *out, size_t capacity, size_t *written) {
    if (!lab || !out || !written) return -1;
    *written = 0;
    if (lab->mode == 2 || lab->mode == 3) {
        if (lab->wire.stage == PLTR_PAIR_WIRE_DONE)
            return now_ms - lab->received_ms > 10000 ? -1 : 0;
        if (lab->mode == 3 && lab->wire.stage == PLTR_PAIR_WIRE_START &&
            now_ms - lab->started_ms > 10000) return -1;
        int result = pltr_pair_wire_tick(&lab->wire, now_ms, out, capacity, written);
        if (result != 0 || lab->mode != 3 || lab->wire.stage != PLTR_PAIR_WIRE_KEYS) return result;
        if ((lab->held && now_ms - lab->pressed_ms > 1000) ||
            (!lab->held && lab->presses && now_ms - lab->released_ms > 2000)) clear_presses(lab);
        if (lab->status_dirty || now_ms - lab->status_ms >= 1000)
            return approval_status(lab, now_ms, out, capacity, written);
        return 0;
    }
    if (!lab->mode) {
        (void)pltr_pairing_tick(&lab->pairing, now_ms);
        return lab->started_ms && now_ms - lab->started_ms > 10000 ? -1 : 0;
    }
    if (lab->link.stage == PLTR_LINK_CLOSED)
        return now_ms - lab->received_ms > 10000 ? -1 : 0;
    if (lab->link.stage != PLTR_LINK_READY)
        return now_ms - lab->started_ms > 10000 ? -1 : 0;
    if (now_ms - lab->received_ms > 10000) return -1;
    if (now_ms - lab->ping_ms < 1000) return 0;
    uint8_t ping[16] = {0};
    for (unsigned i = 0; i < 8; ++i)
        ping[i] = ping[8+i] = (uint8_t)((now_ms * 1000) >> (8*i));
    lab->ping_ms = now_ms;
    return pltr_link_send(&lab->link, PLTR_PING, ping, sizeof(ping),
                           out, capacity, written) == 0 ? 1 : -1;
}

int pltr_ble_lab_observing(const PltrBleLab *lab) {
    return lab && lab->mode == 1 && lab->link.stage == PLTR_LINK_READY &&
           lab->link.relay_session.stage == PLTR_RELAY_OBSERVING;
}

int pltr_ble_lab_approval_pending(const PltrBleLab *lab) {
    if (!lab || lab->mode != 3 || lab->wire.stage != PLTR_PAIR_WIRE_KEYS) return 0;
    return pltr_identity_store_approve((void *)&lab->store, lab->pairing.client_key) ? 2 : 1;
}

int pltr_ble_lab_sample(PltrBleLab *lab, const uint8_t *payload, size_t size,
    uint8_t *out, size_t capacity, size_t *written) {
    if (!pltr_ble_lab_observing(lab)) return -1;
    return pltr_link_send(&lab->link, PLTR_INPUT_SAMPLE, payload, size,
                           out, capacity, written);
}
