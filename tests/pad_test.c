#include "pad.h"

#include <assert.h>
#include <linux/input.h>

int main(void) {
    assert(pltr_pad_product_matches(0, 0x0357));
    assert(pltr_pad_product_matches(0, 0x0360));
    assert(!pltr_pad_product_matches(0, 0x0315));
    assert(pltr_pad_product_matches(0x0357, 0x0357));
    assert(!pltr_pad_product_matches(0x0357, 0x0360));
    PltrPad pad = {.fd = -1};
    uint8_t key;
    assert(pltr_pad_feed(&pad, BTN_0, 1, 100, &key) == 1 && key == 1);
    assert(pltr_pad_feed(&pad, BTN_7, 1, 200, &key) == 1 && key == 8);
    assert(pltr_pad_chord(&pad, 5199) == 0);
    assert(pltr_pad_chord(&pad, 5200) == 1);
    assert(pltr_pad_chord(&pad, 5201) == 0);
    assert(pltr_pad_chord(&pad, 15200) == 2);
    assert(pltr_pad_chord(&pad, 15201) == 0);
    assert(pltr_pad_feed(&pad, BTN_7, 0, 16000, &key) == 0);
    assert(pltr_pad_chord(&pad, 20000) == 0);
    assert(pltr_pad_feed(&pad, BTN_7, 1, 21000, &key) == 1 && key == 8);
    assert(pltr_pad_chord(&pad, 26000) == 1);
    assert(pltr_pad_feed(&pad, BTN_2, 1, 26001, &key) == 1 && key == 3);
    assert(pltr_pad_feed(&pad, BTN_2, 2, 26002, &key) == 0);
    assert(pltr_pad_feed(&pad, BTN_2, 0, 26003, &key) == 0);
    pltr_pad_close(&pad);
    return 0;
}
