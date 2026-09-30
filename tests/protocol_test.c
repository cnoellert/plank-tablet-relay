#include "protocol.h"
#include "../vendor/plank-client/plank.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static size_t make(uint8_t *out, uint16_t type, const uint8_t *payload, size_t n) {
    assert(n <= PLTR_MAX_PAYLOAD_SIZE);
    le32(out, PLTR_MAGIC); le16(out + 4, PLTR_VERSION);
    le16(out + 6, type); le32(out + 8, 1); le32(out + 12, (uint32_t)n);
    if (n) memcpy(out + PLTR_HEADER_SIZE, payload, n);
    return PLTR_HEADER_SIZE + n;
}
static void check(uint16_t type, PltrDirection direction, PltrPhase phase,
                  const uint8_t *payload, size_t n, int valid) {
    uint8_t bytes[PLTR_MAX_FRAME_SIZE];
    PltrFrame decoded = {0};
    size_t size = make(bytes, type, payload, n);
    assert((pltr_decode_frame(bytes, size, direction, phase, 1, &decoded) == 0) == valid);
    if (valid) {
        assert(decoded.type == type && decoded.sequence == 1 &&
               decoded.payload == bytes + PLTR_HEADER_SIZE && decoded.payload_size == n);
        assert(pltr_decode_frame(bytes, size, direction, phase, 2, &decoded) != 0);
    }
    le32(bytes + 12, (uint32_t)(n + 1));
    assert(pltr_decode_frame(bytes, size, direction, phase, 1, &decoded) != 0);
}
int main(void) {
    const uint8_t open[] = {1};
    const uint8_t button_open[] = {3};
    const uint8_t bad_open[] = {4};
    check(PLTR_OPEN, PLTR_CLIENT_TO_RELAY, PLTR_PRE_AUTH, button_open, 1, 1);
    uint8_t approval[] = {1, 1, 2, 3, 60, 0, 8, 1};
    check(PLTR_PAIR_APPROVAL, PLTR_RELAY_TO_CLIENT, PLTR_PRE_AUTH, approval, 8, 1);
    check(PLTR_PAIR_APPROVAL, PLTR_CLIENT_TO_RELAY, PLTR_PRE_AUTH, approval, 8, 0);
    approval[2] = 4;
    check(PLTR_PAIR_APPROVAL, PLTR_RELAY_TO_CLIENT, PLTR_PRE_AUTH, approval, 8, 0);
    check(PLTR_OPEN, PLTR_CLIENT_TO_RELAY, PLTR_PRE_AUTH, open, 1, 1);
    check(PLTR_OPEN, PLTR_RELAY_TO_CLIENT, PLTR_PRE_AUTH, open, 1, 0);
    check(PLTR_OPEN, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, open, 1, 0);
    check(PLTR_OPEN, PLTR_CLIENT_TO_RELAY, PLTR_PRE_AUTH, bad_open, 1, 0);
    check(PLTR_NOISE, PLTR_RELAY_TO_CLIENT, PLTR_PRE_AUTH, open, 1, 1);
    check(PLTR_NOISE, PLTR_RELAY_TO_CLIENT, PLTR_PRE_AUTH, NULL, 0, 0);
    uint8_t pair_start[81] = {0};
    check(PLTR_PAIR_START, PLTR_CLIENT_TO_RELAY, PLTR_PRE_AUTH,
          pair_start, sizeof(pair_start), 1);
    pair_start[80] = 1;
    check(PLTR_PAIR_START, PLTR_CLIENT_TO_RELAY, PLTR_PRE_AUTH,
          pair_start, sizeof(pair_start), 0);
    uint8_t pair_response[66] = {0};
    pair_response[64] = 1; pair_response[65] = 'R';
    check(PLTR_PAIR_RESPONSE, PLTR_RELAY_TO_CLIENT, PLTR_PRE_AUTH,
          pair_response, sizeof(pair_response), 1);
    pair_response[65] = 0xff;
    check(PLTR_PAIR_RESPONSE, PLTR_RELAY_TO_CLIENT, PLTR_PRE_AUTH,
          pair_response, sizeof(pair_response), 0);
    uint8_t mac[32] = {0};
    check(PLTR_PAIR_CONFIRM, PLTR_CLIENT_TO_RELAY, PLTR_PRE_AUTH, mac, 32, 1);
    const uint8_t failed[] = {2};
    check(PLTR_PAIR_RESULT, PLTR_RELAY_TO_CLIENT, PLTR_PRE_AUTH, failed, 1, 1);
    const uint8_t invalid_result[] = {0};
    check(PLTR_PAIR_RESULT, PLTR_RELAY_TO_CLIENT, PLTR_PRE_AUTH, invalid_result, 1, 0);

    uint8_t hello[14] = {0};
    le16(hello, 1); le16(hello + 2, 1); hello[4] = 2;
    le32(hello + 8, 1); hello[12] = 1; hello[13] = 'C';
    check(PLTR_HELLO, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, hello, sizeof(hello), 1);
    check(PLTR_HELLO, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, hello, sizeof(hello), 0);
    hello[4] = 1;
    check(PLTR_HELLO, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, hello, sizeof(hello), 1);
    hello[5] = 1;
    check(PLTR_HELLO, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, hello, sizeof(hello), 0);
    uint8_t ready[5] = {0}; le32(ready, 0x24); ready[4] = 1;
    check(PLTR_SESSION_READY, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, ready, 5, 1);
    le32(ready, 0x24 | PLTR_FEATURE_FRAME_BATCH);
    check(PLTR_SESSION_READY, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, ready, 5, 1);
    le32(ready, 0x04);
    check(PLTR_SESSION_READY, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, ready, 5, 0);
    check(PLTR_SESSION_ACTIVE, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, open, 1, 1);
    check(PLTR_RECONNECT_BEGIN, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, NULL, 0, 1);
    check(PLTR_RECONNECT_FINISH, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, NULL, 0, 1);
    const uint8_t end[] = {2};
    check(PLTR_SESSION_END, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, end, 1, 1);
    uint8_t status[9] = {3, 0, 0, 0, 0, 1, 1, 1, 'W'};
    check(PLTR_STATUS, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, status, 9, 1);
    status[8] = 0xc0;
    check(PLTR_STATUS, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, status, 9, 0);
    uint8_t ping[16] = {0}, pong[32] = {0};
    check(PLTR_PING, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, ping, 16, 1);
    check(PLTR_PONG, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, pong, 32, 1);
    const uint8_t goodbye[] = {3, 0};
    check(PLTR_GOODBYE, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, goodbye, 2, 1);
    check(0xffff, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, NULL, 0, 0);

    uint8_t hid[sizeof(PLANK_RAW_HID_WIRE_HEADER)] = {0};
    le32(hid, PLANK_RAW_HID_WIRE_MAGIC); le16(hid + 4, PLANK_RAW_HID_WIRE_VERSION);
    le16(hid + 6, PLANK_RAW_HID_GET_REPORT);
    check(PLTR_HOST_FRAME, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, hid, sizeof(hid), 1);
    le16(hid + 8, PLANK_RAW_HID_MAX_INTERFACES);
    check(PLTR_HOST_FRAME, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, hid, sizeof(hid), 0);
    le16(hid + 8, 0);
    le32(hid + 16, 1);
    check(PLTR_HOST_FRAME, PLTR_CLIENT_TO_RELAY, PLTR_SECURE, hid, sizeof(hid), 0);
    le32(hid + 16, 0); le16(hid + 6, PLANK_RAW_HID_INPUT);
    uint8_t timed[8 + sizeof(hid)] = {0};
    timed[0] = 1; memcpy(timed + 8, hid, sizeof(hid));
    check(PLTR_CLIENT_FRAME, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, timed, sizeof(timed), 1);
    le16(timed + 8 + 6, PLANK_RAW_HID_SUSPEND);
    check(PLTR_CLIENT_FRAME, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, timed, sizeof(timed), 0);
    timed[0] = 0;
    check(PLTR_CLIENT_FRAME, PLTR_RELAY_TO_CLIENT, PLTR_SECURE, timed, sizeof(timed), 1);
    uint8_t batch[1 + 2 * (10 + sizeof(hid))] = {0};
    batch[0] = 2;
    for (unsigned i = 0; i < 2; ++i) {
        size_t offset = 1 + i * (10 + sizeof(hid));
        batch[offset] = (uint8_t)(i + 1);
        le16(batch + offset + 8, sizeof(hid));
        memcpy(batch + offset + 10, hid, sizeof(hid));
        le16(batch + offset + 10 + 6, PLANK_RAW_HID_INPUT);
    }
    check(PLTR_CLIENT_FRAME_BATCH, PLTR_RELAY_TO_CLIENT, PLTR_SECURE,
          batch, sizeof(batch), 1);
    check(PLTR_CLIENT_FRAME_BATCH, PLTR_RELAY_TO_CLIENT, PLTR_SECURE,
          batch, sizeof(batch) - 1, 0);
    batch[0] = 1;
    check(PLTR_CLIENT_FRAME_BATCH, PLTR_RELAY_TO_CLIENT, PLTR_SECURE,
          batch, sizeof(batch), 0);
    batch[0] = 2;
    batch[1 + 10 + 6] = PLANK_RAW_HID_SUSPEND;
    check(PLTR_CLIENT_FRAME_BATCH, PLTR_RELAY_TO_CLIENT, PLTR_SECURE,
          batch, sizeof(batch), 0);

    uint8_t record[2 + PLTR_HEADER_SIZE + 1] = {0};
    le16(record, PLTR_HEADER_SIZE + 1);
    make(record + 2, PLTR_OPEN, open, 1);
    PltrFrame decoded = {0};
    assert(pltr_decode_record(record, sizeof(record), PLTR_CLIENT_TO_RELAY,
                              PLTR_PRE_AUTH, 1, &decoded) == 0);
    assert(pltr_decode_record(record, sizeof(record) - 1, PLTR_CLIENT_TO_RELAY,
                              PLTR_PRE_AUTH, 1, &decoded) != 0);
    record[2] = 0;
    assert(pltr_decode_record(record, sizeof(record), PLTR_CLIENT_TO_RELAY,
                              PLTR_PRE_AUTH, 1, &decoded) != 0);

    PltrRecordReader reader;
    const uint8_t *body = NULL;
    size_t consumed = 0, body_size = 0;
    pltr_record_reader_init(&reader, PLTR_PRE_AUTH);
    record[2] = 'R';
    assert(pltr_record_reader_push(&reader, record, 1, &consumed,
                                   &body, &body_size) == 0 && consumed == 1);
    assert(pltr_record_reader_set_phase(&reader, PLTR_SECURE) != 0);
    assert(pltr_record_reader_push(&reader, record + 1, sizeof(record) - 1,
                                   &consumed, &body, &body_size) == 1);
    assert(consumed == sizeof(record) - 1 && body_size == PLTR_HEADER_SIZE + 1);
    assert(pltr_decode_frame(body, body_size, PLTR_CLIENT_TO_RELAY,
                             PLTR_PRE_AUTH, 1, &decoded) == 0);
    assert(pltr_record_reader_set_phase(&reader, PLTR_SECURE) == 0);
    uint8_t two_records[2 * sizeof(record)];
    memcpy(two_records, record, sizeof(record));
    memcpy(two_records + sizeof(record), record, sizeof(record));
    pltr_record_reader_init(&reader, PLTR_PRE_AUTH);
    assert(pltr_record_reader_push(&reader, two_records, sizeof(two_records),
                                   &consumed, &body, &body_size) == 1);
    assert(consumed == sizeof(record));
    assert(pltr_record_reader_push(&reader, two_records + consumed,
                                   sizeof(two_records) - consumed,
                                   &consumed, &body, &body_size) == 1);
    assert(consumed == sizeof(record));
    const uint8_t oversized[] = {0xff, 0xff};
    assert(pltr_record_reader_push(&reader, oversized, sizeof(oversized),
                                   &consumed, &body, &body_size) == -1);
    return 0;
}
