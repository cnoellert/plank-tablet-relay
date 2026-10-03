/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See drawing_status.h. Portable: libc only. */
#include "drawing_status.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static int digits_and_dots(const char *text, size_t length) {
    size_t index;
    if (length == 0) return 0;
    for (index = 0; index < length; ++index)
        if (!((text[index] >= '0' && text[index] <= '9') || text[index] == '.'))
            return 0;
    return 1;
}

/* Section 7.4a step 1: a pure-string dotted-quad grammar. A text that passes is
 * canonical by construction, so no round-trip through a platform parser. */
static PltrBoundAddressResult parse_dotted_quad(const char *text, size_t length,
                                                uint8_t octets[4]) {
    size_t index = 0;
    int group;
    for (group = 0; group < 4; ++group) {
        size_t digits = 0;
        unsigned value = 0;
        if (group > 0) {
            if (index >= length || text[index] != '.') return PLTR_BOUND_ADDRESS_NOT_LITERAL;
            ++index;
        }
        while (index < length && text[index] >= '0' && text[index] <= '9') {
            if (digits == 3) return PLTR_BOUND_ADDRESS_NOT_LITERAL;
            value = value * 10u + (unsigned)(text[index] - '0');
            ++digits;
            ++index;
        }
        if (digits == 0 || value > 255u) return PLTR_BOUND_ADDRESS_NOT_LITERAL;
        if (digits > 1 && text[index - digits] == '0')
            return PLTR_BOUND_ADDRESS_NON_CANONICAL;
        octets[group] = (uint8_t)value;
    }
    if (index != length) return PLTR_BOUND_ADDRESS_NOT_LITERAL;
    return PLTR_BOUND_ADDRESS_OK;
}

/* Section 7.4a steps 2, 4 and 5 for an IPv6 candidate. */
static PltrBoundAddressResult classify_v6(const char *text, size_t length) {
    unsigned char bytes[16];
    char rendered[INET6_ADDRSTRLEN];
    size_t index;
    uint32_t tail;
    for (index = 0; index < length; ++index)
        if (text[index] >= 'A' && text[index] <= 'Z')
            return PLTR_BOUND_ADDRESS_NON_CANONICAL;
    if (inet_pton(AF_INET6, text, bytes) != 1) return PLTR_BOUND_ADDRESS_NOT_LITERAL;
    for (index = 0; index < 10; ++index)
        if (bytes[index] != 0) break;
    if (index == 10 && bytes[10] == 0xff && bytes[11] == 0xff)
        return PLTR_BOUND_ADDRESS_MAPPED;
    for (index = 0; index < 12; ++index)
        if (bytes[index] != 0) break;
    if (index == 12) {
        tail = ((uint32_t)bytes[12] << 24) | ((uint32_t)bytes[13] << 16) |
               ((uint32_t)bytes[14] << 8) | (uint32_t)bytes[15];
        if (tail > 1u) return PLTR_BOUND_ADDRESS_MAPPED;
    }
    if (inet_ntop(AF_INET6, bytes, rendered, sizeof(rendered)) == NULL)
        return PLTR_BOUND_ADDRESS_NOT_LITERAL;
    if (strcmp(rendered, text) != 0) return PLTR_BOUND_ADDRESS_NON_CANONICAL;
    return PLTR_BOUND_ADDRESS_FAMILY_UNSUPPORTED;
}

PltrBoundAddressResult pltr_drawing_status_check_bound_address(const char *text) {
    size_t length;
    uint8_t octets[4];
    PltrBoundAddressResult result;
    if (text == NULL) return PLTR_BOUND_ADDRESS_MISSING;
    length = strlen(text);
    if (length > 15) return PLTR_BOUND_ADDRESS_TOO_LONG;
    if (digits_and_dots(text, length)) {
        result = parse_dotted_quad(text, length, octets);
        if (result != PLTR_BOUND_ADDRESS_OK) return result;
        /* Section 7.5 step 6. The unspecified address itself is accepted; the
         * rest of 0/8, all of 224/4 and 240/4 and the broadcast address are not.
         * Loopback and 169.254/16 are accepted here and refused at conversion. */
        if (octets[0] == 0) {
            if (octets[1] == 0 && octets[2] == 0 && octets[3] == 0)
                return PLTR_BOUND_ADDRESS_OK;
            return PLTR_BOUND_ADDRESS_INVALID;
        }
        if (octets[0] >= 224) return PLTR_BOUND_ADDRESS_INVALID;
        return PLTR_BOUND_ADDRESS_OK;
    }
    if (memchr(text, ':', length) != NULL) return classify_v6(text, length);
    return PLTR_BOUND_ADDRESS_NOT_LITERAL;
}

const char *pltr_drawing_status_bound_address_reason(PltrBoundAddressResult result) {
    switch (result) {
    case PLTR_BOUND_ADDRESS_OK: return NULL;
    case PLTR_BOUND_ADDRESS_MISSING: return "metadata.boundAddress.missing";
    case PLTR_BOUND_ADDRESS_TOO_LONG: return "metadata.boundAddress.tooLong";
    case PLTR_BOUND_ADDRESS_NOT_LITERAL: return "metadata.boundAddress.notLiteral";
    case PLTR_BOUND_ADDRESS_NON_CANONICAL: return "metadata.boundAddress.nonCanonical";
    case PLTR_BOUND_ADDRESS_MAPPED: return "metadata.boundAddress.mapped";
    case PLTR_BOUND_ADDRESS_FAMILY_UNSUPPORTED:
        return "metadata.boundAddress.familyUnsupported";
    case PLTR_BOUND_ADDRESS_INVALID: return "metadata.boundAddress.invalid";
    }
    return "metadata.boundAddress.invalid";
}

int pltr_drawing_status_peer_accepted(uint32_t peer_uid, uint32_t accepted_uid) {
    return peer_uid == accepted_uid ? 1 : 0;
}

void pltr_drawing_status_identity_hex(const uint8_t public_key[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0; index < 32; ++index) {
        out[index * 2] = digits[public_key[index] >> 4];
        out[index * 2 + 1] = digits[public_key[index] & 0x0f];
    }
    out[64] = '\0';
}

int pltr_drawing_status_ready(const uint8_t public_key[32],
                             const char *bound_address, uint16_t bound_port,
                             char *out, size_t capacity) {
    char identity[65];
    int written;
    if (public_key == NULL || out == NULL || bound_port == 0) return -1;
    if (pltr_drawing_status_check_bound_address(bound_address) != PLTR_BOUND_ADDRESS_OK)
        return -1;
    if (capacity > PLTR_DRAWING_STATUS_RESPONSE_MAX)
        capacity = PLTR_DRAWING_STATUS_RESPONSE_MAX;
    pltr_drawing_status_identity_hex(public_key, identity);
    written = snprintf(out, capacity,
        "{\"version\":1,\"ok\":true,\"supported\":true,\"state\":\"ready\","
        "\"listener\":{\"drawingIdentity\":\"%s\","
        "\"drawingProtocol\":{\"name\":\"pltr-raw-hid\",\"version\":1,"
        "\"rawHID\":1,\"linkType\":2},"
        "\"boundAddress\":\"%s\",\"boundPort\":%u}}\n",
        identity, bound_address, (unsigned)bound_port);
    if (written < 0 || (size_t)written >= capacity) return -1;
    return written;
}

/* Reason syntax, contract section 3: 1 to 64 bytes of [a-zA-Z.] only. */
static int reason_well_formed(const char *reason) {
    size_t length, index;
    if (reason == NULL) return 0;
    length = strlen(reason);
    if (length == 0 || length > 64) return 0;
    for (index = 0; index < length; ++index) {
        const char character = reason[index];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') || character == '.'))
            return 0;
    }
    return 1;
}

int pltr_drawing_status_unavailable(const char *reason, char *out, size_t capacity) {
    int written;
    if (out == NULL || !reason_well_formed(reason)) return -1;
    if (capacity > PLTR_DRAWING_STATUS_RESPONSE_MAX)
        capacity = PLTR_DRAWING_STATUS_RESPONSE_MAX;
    written = snprintf(out, capacity,
        "{\"version\":1,\"ok\":true,\"supported\":true,"
        "\"state\":\"unavailable\",\"reason\":\"%s\"}\n", reason);
    if (written < 0 || (size_t)written >= capacity) return -1;
    return written;
}

int pltr_drawing_status_error(const char *text, char *out, size_t capacity) {
    size_t length, index;
    int written;
    if (out == NULL || text == NULL) return -1;
    length = strlen(text);
    if (length == 0 || length > 64) return -1;
    /* Printable ASCII without the two characters JSON would need escaped. The
     * text is a fixed in-tree string; no peer input ever reaches it. */
    for (index = 0; index < length; ++index) {
        const unsigned char character = (unsigned char)text[index];
        if (character < 0x20 || character > 0x7e || character == '"' ||
            character == '\\')
            return -1;
    }
    if (capacity > PLTR_DRAWING_STATUS_RESPONSE_MAX)
        capacity = PLTR_DRAWING_STATUS_RESPONSE_MAX;
    written = snprintf(out, capacity,
        "{\"version\":1,\"ok\":false,\"error\":\"%s\"}\n", text);
    if (written < 0 || (size_t)written >= capacity) return -1;
    return written;
}

static size_t skip_space(const char *bytes, size_t length, size_t index) {
    while (index < length && (bytes[index] == ' ' || bytes[index] == '\t' ||
                              bytes[index] == '\r' || bytes[index] == '\n'))
        ++index;
    return index;
}

/* Reads a bare JSON string with no escapes. Escapes are refused rather than
 * decoded: no legal member name or value in this request needs one. */
static int read_plain_string(const char *bytes, size_t length, size_t *index,
                             char *out, size_t out_capacity) {
    size_t cursor = *index, written = 0;
    if (cursor >= length || bytes[cursor] != '"') return 0;
    ++cursor;
    while (cursor < length && bytes[cursor] != '"') {
        const unsigned char character = (unsigned char)bytes[cursor];
        if (character < 0x20 || character == '\\') return 0;
        if (written + 1 >= out_capacity) return 0;
        out[written++] = (char)character;
        ++cursor;
    }
    if (cursor >= length) return 0;
    out[written] = '\0';
    *index = cursor + 1;
    return 1;
}

int pltr_drawing_status_check_request(const char *bytes, size_t length) {
    size_t index = 0;
    int seen_op = 0, seen_version = 0;
    if (bytes == NULL || length < 2 || length > PLTR_DRAWING_STATUS_REQUEST_MAX)
        return 0;
    if (bytes[length - 1] == '\n') --length;
    index = skip_space(bytes, length, index);
    if (index >= length || bytes[index] != '{') return 0;
    ++index;
    for (;;) {
        char name[32], value[32];
        index = skip_space(bytes, length, index);
        if (index < length && bytes[index] == '}') { ++index; break; }
        if (seen_op || seen_version) {
            if (index >= length || bytes[index] != ',') return 0;
            ++index;
            index = skip_space(bytes, length, index);
        }
        if (!read_plain_string(bytes, length, &index, name, sizeof(name))) return 0;
        index = skip_space(bytes, length, index);
        if (index >= length || bytes[index] != ':') return 0;
        ++index;
        index = skip_space(bytes, length, index);
        if (strcmp(name, "op") == 0) {
            if (seen_op) return 0;
            if (!read_plain_string(bytes, length, &index, value, sizeof(value)))
                return 0;
            if (strcmp(value, "drawing-status") != 0) return 0;
            seen_op = 1;
        } else if (strcmp(name, "version") == 0) {
            if (seen_version) return 0;
            if (index >= length || bytes[index] != '1') return 0;
            ++index;
            if (index < length && ((bytes[index] >= '0' && bytes[index] <= '9') ||
                                   bytes[index] == '.' || bytes[index] == 'e' ||
                                   bytes[index] == 'E'))
                return 0;
            seen_version = 1;
        } else {
            return 0;
        }
    }
    index = skip_space(bytes, length, index);
    return index == length && seen_op && seen_version ? 1 : 0;
}
