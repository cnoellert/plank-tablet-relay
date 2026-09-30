#define _POSIX_C_SOURCE 200809L
#include "pad.h"

#include <poll.h>
#include <stdio.h>
#include <time.h>

static uint64_t monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

int main(void) {
    PltrPad pad;
    if (pltr_pad_open(&pad, 0x056a, 0) != 0) {
        fputs("Wacom pad input unavailable\n", stderr);
        return 1;
    }
    puts("Wacom pad input ready; reading keys for 90 seconds");
    fflush(stdout);
    const uint64_t end = monotonic_ms() + 90000;
    while (monotonic_ms() < end) {
        struct pollfd pfd = {pad.fd, POLLIN, 0};
        int ready = poll(&pfd, 1, 100);
        if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP))) break;
        if (ready > 0 && (pfd.revents & POLLIN)) {
            uint8_t key;
            int result = pltr_pad_read(&pad, monotonic_ms(), &key);
            if (result < 0) break;
            if (result == 1) {
                printf("ExpressKey %u\n", key);
                fflush(stdout);
            }
        }
        int chord = pltr_pad_chord(&pad, monotonic_ms());
        if (chord) {
            printf("Chord action %d\n", chord);
            fflush(stdout);
        }
    }
    pltr_pad_close(&pad);
    return 0;
}
