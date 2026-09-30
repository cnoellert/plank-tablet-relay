#define _POSIX_C_SOURCE 200809L
#include "pad.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int has_key(const uint8_t *bits, unsigned key) {
    return (bits[key / 8] & (uint8_t)(1u << (key % 8))) != 0;
}

int pltr_pad_product_matches(uint16_t requested, uint16_t actual) {
    return requested == 0 ? actual == 0x0357 || actual == 0x0360 :
                            actual == requested;
}

int pltr_pad_open(PltrPad *pad, uint16_t vendor, uint16_t product) {
    if (pad == NULL || vendor == 0) return -1;
    memset(pad, 0, sizeof(*pad));
    pad->fd = -1;
    DIR *input = opendir("/dev/input");
    if (input == NULL) return -1;
    struct dirent *entry;
    while ((entry = readdir(input)) != NULL) {
        const char *suffix = entry->d_name;
        if (strncmp(suffix, "event", 5) != 0) continue;
        suffix += 5;
        if (*suffix == '\0') continue;
        int numeric = 1;
        for (const char *p = suffix; *p; ++p)
            if (*p < '0' || *p > '9') numeric = 0;
        if (!numeric) continue;
        char path[32], name[256] = {0};
        int length = snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (length < 0 || length >= (int)sizeof(path)) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        struct input_id id;
        uint8_t keys[BTN_7 / 8 + 1] = {0};
        if (ioctl(fd, EVIOCGID, &id) == 0 && id.vendor == vendor &&
            pltr_pad_product_matches(product, id.product) &&
            ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            strstr(name, "Pad") != NULL &&
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 &&
            has_key(keys, BTN_0) && has_key(keys, BTN_7)) {
            if (pad->fd >= 0) {
                // USB and Bluetooth may both be present. Never choose an
                // arbitrary Pad for physical pairing or the idle chord.
                close(fd);
                pltr_pad_close(pad);
                closedir(input);
                return -1;
            }
            pad->fd = fd;
            continue;
        }
        close(fd);
    }
    closedir(input);
    return pad->fd >= 0 ? 0 : -1;
}

void pltr_pad_close(PltrPad *pad) {
    if (pad == NULL) return;
    if (pad->fd >= 0) close(pad->fd);
    memset(pad, 0, sizeof(*pad));
    pad->fd = -1;
}

int pltr_pad_feed(PltrPad *pad, unsigned code, int value,
                   uint64_t now_ms, uint8_t *key) {
    if (pad == NULL || key == NULL) return -1;
    *key = 0;
    if (code < BTN_0 || code > BTN_7 ||
        (value != 0 && value != 1)) return 0;
    const unsigned index = code - BTN_0;
    const int was_chord = pad->held[0] && pad->held[7];
    if (value == 1 && !pad->held[index]) {
        pad->held[index] = 1;
        *key = (uint8_t)(index + 1);
    } else if (value == 0) {
        pad->held[index] = 0;
    }
    const int is_chord = pad->held[0] && pad->held[7];
    if (!was_chord && is_chord) {
        pad->chord_since_ms = now_ms;
        pad->chord_active = 1;
        pad->chord_signal = 0;
    } else if (was_chord && !is_chord) {
        pad->chord_active = 0;
        pad->chord_signal = 0;
    }
    return *key ? 1 : 0;
}

int pltr_pad_chord(PltrPad *pad, uint64_t now_ms) {
    if (pad == NULL || !pad->chord_active ||
        now_ms < pad->chord_since_ms) return 0;
    const uint64_t elapsed = now_ms - pad->chord_since_ms;
    if (elapsed >= 15000 && pad->chord_signal < 2) {
        pad->chord_signal = 2;
        return 2;
    }
    if (elapsed >= 5000 && pad->chord_signal < 1) {
        pad->chord_signal = 1;
        return 1;
    }
    return 0;
}

int pltr_pad_read(PltrPad *pad, uint64_t now_ms, uint8_t *key) {
    if (pad == NULL || key == NULL || pad->fd < 0) return -1;
    *key = 0;
    struct input_event event;
    ssize_t size = read(pad->fd, &event, sizeof(event));
    if (size < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (size != (ssize_t)sizeof(event)) return -1;
    if (event.type != EV_KEY) return 0;
    return pltr_pad_feed(pad, event.code, event.value, now_ms, key);
}
