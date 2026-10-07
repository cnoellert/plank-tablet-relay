#ifndef PLANK_RELAY_PAD_H
#define PLANK_RELAY_PAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PltrPad {
    int fd;
    uint8_t held[8];
    uint64_t chord_since_ms;
    unsigned chord_active, chord_signal;
} PltrPad;

// Opens exactly one matching Wacom Pad evdev node for reads only, without
// grabbing it. A zero product accepts the PTH-660's verified USB (0357) and
// Bluetooth (0360) product IDs; multiple eligible Pads fail closed. The idle
// reader is nonexclusive; a pairing window obtains the shared capture lease.
int pltr_pad_open(PltrPad *pad, uint16_t vendor, uint16_t product);
// Pure product policy for tests. 0 means the two verified PTH-660 identities.
int pltr_pad_product_matches(uint16_t requested, uint16_t actual);
void pltr_pad_close(PltrPad *pad);

// Receive one input event, returning 1 for an ExpressKey down (key 1..8),
// 0 for no key down, -1 for a lost device/read error. The caller feeds key
// downs to the pairing engine only in its WAIT_CODE state.
int pltr_pad_read(PltrPad *pad, uint64_t now_ms, uint8_t *key);

// Pure event path for tests. BTN_0..BTN_7 map to digits 1..8. 0 means no
// digit, 1 means a key-down digit was emitted.
int pltr_pad_feed(PltrPad *pad, unsigned code, int value,
                   uint64_t now_ms, uint8_t *key);

// Holding keys 1 and 8 together emits 1 after 5 seconds (open pairing),
// then 2 after 15 seconds (forget all). Each action fires once per hold.
int pltr_pad_chord(PltrPad *pad, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
