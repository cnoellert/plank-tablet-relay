#pragma once

#include <cstdint>

struct PlankWacomHidIdentity {
    std::uint16_t bus;
    std::uint16_t product;
};

// Linux HID_ID is three hexadecimal fields: bus:vendor:product. Keep this
// strict so a similarly named non-Wacom HID device cannot enter raw capture.
inline bool plankParseWacomHidIdentity(const char* text,
                                      PlankWacomHidIdentity* identity)
{
    if (text == nullptr || identity == nullptr) return false;
    std::uint32_t fields[3] = {};
    for (unsigned field = 0; field < 3; ++field) {
        unsigned digits = 0;
        while (*text != '\0' && *text != ':') {
            unsigned digit;
            if (*text >= '0' && *text <= '9') digit = *text - '0';
            else if (*text >= 'a' && *text <= 'f') digit = *text - 'a' + 10;
            else if (*text >= 'A' && *text <= 'F') digit = *text - 'A' + 10;
            else return false;
            if (++digits > 8) return false;
            fields[field] = (fields[field] << 4) | digit;
            ++text;
        }
        if (digits == 0 || (field < 2 && *text++ != ':')) return false;
        if (field == 2 && *text != '\0') return false;
    }
    if (fields[0] != 0x0005 || fields[1] != 0x056a ||
        fields[2] > 0xffffu) return false;
    *identity = {static_cast<std::uint16_t>(fields[0]),
                 static_cast<std::uint16_t>(fields[2])};
    return true;
}
