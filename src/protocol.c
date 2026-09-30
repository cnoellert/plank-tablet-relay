#include "protocol.h"
#include "../vendor/plank-client/plank.h"

#include <string.h>

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_le16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static int valid_utf8(const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size;) {
        const uint8_t first = bytes[i++];
        if (first == 0) return 0;
        if (first < 0x80) continue;
        unsigned count;
        uint32_t codepoint;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            codepoint = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            codepoint = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            codepoint = first & 0x07;
        } else {
            return 0;
        }
        if (count > size - i) return 0;
        for (unsigned j = 0; j < count; ++j) {
            if ((bytes[i] & 0xc0) != 0x80) return 0;
            codepoint = (codepoint << 6) | (bytes[i++] & 0x3f);
        }
        if ((count == 1 && codepoint < 0x80) ||
            (count == 2 && codepoint < 0x800) ||
            (count == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint > 0x10ffff) return 0;
    }
    return 1;
}

static int valid_string(const uint8_t *bytes, size_t size, size_t prefix,
                        size_t max_length) {
    if (size < prefix + 1) return 0;
    const size_t length = bytes[prefix];
    return length <= max_length && size == prefix + 1 + length &&
           valid_utf8(bytes + prefix + 1, length);
}

static int valid_plwh(const uint8_t *bytes, size_t size,
                      PltrDirection direction) {
    if (size < sizeof(PLANK_RAW_HID_WIRE_HEADER) ||
        read_le32(bytes) != PLANK_RAW_HID_WIRE_MAGIC ||
        read_le16(bytes + 4) != PLANK_RAW_HID_WIRE_VERSION ||
        read_le16(bytes + 8) >= PLANK_RAW_HID_MAX_INTERFACES ||
        read_le32(bytes + 16) > PLANK_RAW_HID_MAX_PAYLOAD_SIZE ||
        read_le32(bytes + 16) != size - sizeof(PLANK_RAW_HID_WIRE_HEADER)) return 0;
    const uint16_t type = read_le16(bytes + 6);
    if (direction == PLTR_CLIENT_TO_RELAY) {
        return type == PLANK_RAW_HID_ATTACH_RESULT ||
               type == PLANK_RAW_HID_GET_REPORT ||
               type == PLANK_RAW_HID_SET_REPORT ||
               type == PLANK_RAW_HID_OUTPUT ||
               type == PLANK_RAW_HID_OPEN ||
               type == PLANK_RAW_HID_CLOSE;
    }
    return type == PLANK_RAW_HID_DEVICE ||
           type == PLANK_RAW_HID_DESCRIPTOR ||
           type == PLANK_RAW_HID_INPUT ||
           type == PLANK_RAW_HID_GET_REPORT_REPLY ||
           type == PLANK_RAW_HID_SET_REPORT_REPLY ||
           type == PLANK_RAW_HID_DETACH ||
           type == PLANK_RAW_HID_SUSPEND;
}

static int valid_payload(uint16_t type, const uint8_t *bytes, size_t size,
                         PltrDirection direction) {
    switch (type) {
    case PLTR_HELLO:
        return valid_string(bytes, size, 12, 64) &&
               read_le16(bytes) <= PLTR_VERSION &&
               read_le16(bytes + 2) >= PLTR_VERSION &&
               bytes[4] == (direction == PLTR_CLIENT_TO_RELAY ? 2 : 1) &&
               bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 &&
               (read_le32(bytes + 8) & ~3u) == 0 &&
               (read_le32(bytes + 8) & PLTR_FEATURE_RAW_HID) != 0;
    case PLTR_INPUT_OBSERVE:
        return size == 1 && bytes[0] <= 1;
    case PLTR_INPUT_SAMPLE:
        return size == PLTR_INPUT_SAMPLE_SIZE && bytes[0] == 1 &&
               (bytes[1] & ~63u) == 0 && bytes[78] == 0 && bytes[79] == 0;
    case PLTR_SESSION_READY:
        return size == 5 && (read_le32(bytes) & 0x24u) == 0x24u && bytes[4] <= 1;
    case PLTR_SESSION_ACTIVE:
        return size == 1 && bytes[0] <= 1;
    case PLTR_RECONNECT_BEGIN:
    case PLTR_RECONNECT_FINISH:
        return size == 0;
    case PLTR_SESSION_END:
        return size == 1 && bytes[0] >= 1 && bytes[0] <= 2;
    case PLTR_HOST_FRAME:
        return valid_plwh(bytes, size, PLTR_CLIENT_TO_RELAY);
    case PLTR_CLIENT_FRAME:
        return size >= 8 && valid_plwh(bytes + 8, size - 8, PLTR_RELAY_TO_CLIENT) &&
               (read_le16(bytes + 8 + 6) == PLANK_RAW_HID_INPUT ||
                memcmp(bytes, "\0\0\0\0\0\0\0\0", 8) == 0);
    case PLTR_CLIENT_FRAME_BATCH: {
        if (size < 1 || bytes[0] < 2 || bytes[0] > PLTR_MAX_BATCH_FRAMES)
            return 0;
        size_t offset = 1;
        for (unsigned i = 0; i < bytes[0]; ++i) {
            if (size - offset < 10) return 0;
            const size_t frame_size = read_le16(bytes + offset + 8);
            offset += 10;
            if (frame_size > size - offset ||
                !valid_plwh(bytes + offset, frame_size, PLTR_RELAY_TO_CLIENT) ||
                read_le16(bytes + offset + 6) != PLANK_RAW_HID_INPUT)
                return 0;
            offset += frame_size;
        }
        return offset == size;
    }
    case PLTR_STATUS:
        return valid_string(bytes, size, 7, 128) && bytes[0] <= 7 &&
               bytes[5] <= PLANK_RAW_HID_MAX_INTERFACES &&
               bytes[6] >= 1 && bytes[6] <= 2;
    case PLTR_PING:
        return size == 16;
    case PLTR_PONG:
        return size == 32;
    case PLTR_GOODBYE:
        return size == 2 && read_le16(bytes) >= 1 && read_le16(bytes) <= 5;
    case PLTR_OPEN:
        return size == 1 && bytes[0] >= 1 && bytes[0] <= 3;
    case PLTR_NOISE:
        return size >= 1;
    case PLTR_PAIR_START:
        return valid_string(bytes, size, 80, 64);
    case PLTR_PAIR_RESPONSE:
        return valid_string(bytes, size, 64, 64);
    case PLTR_PAIR_CONFIRM:
        return size == 32;
    case PLTR_PAIR_RESULT:
        return (size == 33 && bytes[0] == 0) ||
               (size == 1 && bytes[0] >= 1 && bytes[0] <= 3);
    case PLTR_PAIR_APPROVAL:
        return size == 8 && bytes[0] == 1 && bytes[1] <= 1 &&
               bytes[2] <= 3 && bytes[3] == 3 &&
               read_le16(bytes + 4) <= 60 && read_le16(bytes + 6) < 768;
    default:
        return 0;
    }
}

static int allowed_type(uint16_t type, PltrDirection direction, PltrPhase phase) {
    if (phase == PLTR_PRE_AUTH) {
        if (type == PLTR_NOISE) return 1;
        if (direction == PLTR_CLIENT_TO_RELAY) {
            return type == PLTR_OPEN || type == PLTR_PAIR_START ||
                   type == PLTR_PAIR_CONFIRM;
        }
        if (direction == PLTR_RELAY_TO_CLIENT) {
            return type == PLTR_PAIR_RESPONSE || type == PLTR_PAIR_RESULT ||
                   type == PLTR_PAIR_APPROVAL;
        }
        return 0;
    }
    if (phase != PLTR_SECURE) return 0;
    if (type == PLTR_HELLO || type == PLTR_PING ||
        type == PLTR_PONG || type == PLTR_GOODBYE) {
        return 1;
    }
    if (direction == PLTR_CLIENT_TO_RELAY) {
        return (type >= PLTR_SESSION_READY && type <= PLTR_HOST_FRAME) ||
               type == PLTR_INPUT_OBSERVE;
    }
    if (direction == PLTR_RELAY_TO_CLIENT) {
        return type == PLTR_CLIENT_FRAME || type == PLTR_CLIENT_FRAME_BATCH ||
               type == PLTR_STATUS ||
               type == PLTR_INPUT_SAMPLE;
    }
    return 0;
}

void pltr_record_reader_init(PltrRecordReader *reader, PltrPhase phase) {
    if (reader == NULL) return;
    reader->filled = 0;
    reader->body_size = 0;
    reader->phase = phase;
}

int pltr_record_reader_set_phase(PltrRecordReader *reader, PltrPhase phase) {
    if (reader == NULL || reader->filled != 0 ||
        (phase != PLTR_PRE_AUTH && phase != PLTR_SECURE)) return -1;
    reader->phase = phase;
    return 0;
}

int pltr_record_reader_push(PltrRecordReader *reader, const uint8_t *bytes,
                            size_t size, size_t *consumed,
                            const uint8_t **body, size_t *body_size) {
    if (reader == NULL || consumed == NULL || body == NULL || body_size == NULL ||
        (bytes == NULL && size != 0) ||
        (reader->phase != PLTR_PRE_AUTH && reader->phase != PLTR_SECURE)) return -1;
    *consumed = 0;
    *body = NULL;
    *body_size = 0;
    while (*consumed < size) {
        if (reader->filled < 2) {
            reader->bytes[reader->filled++] = bytes[(*consumed)++];
            if (reader->filled < 2) continue;
            reader->body_size = read_le16(reader->bytes);
            const size_t minimum = reader->phase == PLTR_PRE_AUTH ?
                                   PLTR_HEADER_SIZE : 16;
            const size_t maximum = reader->phase == PLTR_PRE_AUTH ?
                                   PLTR_MAX_FRAME_SIZE : PLTR_MAX_RECORD_BODY_SIZE;
            if (reader->body_size < minimum || reader->body_size > maximum) {
                reader->filled = 0;
                reader->body_size = 0;
                return -1;
            }
        }
        const size_t remaining = reader->body_size + 2 - reader->filled;
        const size_t available = size - *consumed;
        const size_t take = remaining < available ? remaining : available;
        memcpy(reader->bytes + reader->filled, bytes + *consumed, take);
        reader->filled += take;
        *consumed += take;
        if (reader->filled == reader->body_size + 2) {
            *body = reader->bytes + 2;
            *body_size = reader->body_size;
            reader->filled = 0;
            reader->body_size = 0;
            return 1;
        }
    }
    return 0;
}

int pltr_decode_frame(const uint8_t *bytes, size_t size,
                      PltrDirection direction, PltrPhase phase,
                      uint32_t expected_sequence, PltrFrame *out) {
    if (bytes == NULL || out == NULL || size < PLTR_HEADER_SIZE ||
            size > PLTR_MAX_FRAME_SIZE || expected_sequence == 0 ||
            expected_sequence == UINT32_MAX ||
            read_le32(bytes) != PLTR_MAGIC ||
            read_le16(bytes + 4) != PLTR_VERSION ||
            read_le32(bytes + 8) != expected_sequence) {
        return -1;
    }
    const uint16_t type = read_le16(bytes + 6);
    const uint32_t payload_size = read_le32(bytes + 12);
    if (!allowed_type(type, direction, phase) ||
            payload_size != size - PLTR_HEADER_SIZE ||
            !valid_payload(type, bytes + PLTR_HEADER_SIZE, payload_size, direction)) {
        return -1;
    }
    out->type = type;
    out->sequence = expected_sequence;
    out->payload = bytes + PLTR_HEADER_SIZE;
    out->payload_size = payload_size;
    return 0;
}

int pltr_encode_frame(uint16_t type, uint32_t sequence,
                      const uint8_t *payload, size_t payload_size,
                      PltrDirection direction, PltrPhase phase,
                      uint8_t *out, size_t capacity, size_t *written) {
    if (out == NULL || written == NULL ||
        (payload == NULL && payload_size != 0) ||
        sequence == 0 || sequence == UINT32_MAX ||
        payload_size > PLTR_MAX_PAYLOAD_SIZE ||
        capacity < PLTR_HEADER_SIZE + payload_size ||
        !allowed_type(type, direction, phase) ||
        !valid_payload(type, payload, payload_size, direction)) return -1;
    write_le32(out, PLTR_MAGIC);
    write_le16(out + 4, PLTR_VERSION);
    write_le16(out + 6, type);
    write_le32(out + 8, sequence);
    write_le32(out + 12, (uint32_t)payload_size);
    if (payload_size) memcpy(out + PLTR_HEADER_SIZE, payload, payload_size);
    *written = PLTR_HEADER_SIZE + payload_size;
    return 0;
}

int pltr_decode_record(const uint8_t *bytes, size_t size,
                       PltrDirection direction, PltrPhase phase,
                       uint32_t expected_sequence, PltrFrame *out) {
    if (phase != PLTR_PRE_AUTH || bytes == NULL || size < 2 ||
            size > PLTR_MAX_FRAME_SIZE + 2 ||
            read_le16(bytes) != size - 2) {
        return -1;
    }
    return pltr_decode_frame(bytes + 2, size - 2, direction,
                             phase, expected_sequence, out);
}
