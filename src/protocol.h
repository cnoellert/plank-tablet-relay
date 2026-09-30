#ifndef PLANK_RELAY_PROTOCOL_H
#define PLANK_RELAY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLTR_MAGIC 0x504c5452u
#define PLTR_VERSION 1u
#define PLTR_HEADER_SIZE 16u
#define PLTR_MAX_PAYLOAD_SIZE 8192u
#define PLTR_MAX_FRAME_SIZE (PLTR_HEADER_SIZE + PLTR_MAX_PAYLOAD_SIZE)
#define PLTR_MAX_RECORD_BODY_SIZE (PLTR_MAX_FRAME_SIZE + 16u)
#define PLTR_FEATURE_RAW_HID 1u
#define PLTR_FEATURE_INPUT_OBSERVER 2u
#define PLTR_FEATURE_FRAME_BATCH 0x80000000u
#define PLTR_MAX_BATCH_FRAMES 4u
#define PLTR_INPUT_SAMPLE_SIZE 80u
// Public domain value for physically approved lab enrollment, NOT a password.
// This mode has no first-pairing MITM protection. Saved-key Noise is unchanged.
#define PLTR_BUTTON_APPROVAL_CODE "11111"

typedef enum PltrType {
    PLTR_HELLO = 1,
    PLTR_SESSION_READY = 2,
    PLTR_SESSION_ACTIVE = 3,
    PLTR_RECONNECT_BEGIN = 4,
    PLTR_RECONNECT_FINISH = 5,
    PLTR_SESSION_END = 6,
    PLTR_HOST_FRAME = 7,
    PLTR_CLIENT_FRAME = 8,
    PLTR_STATUS = 9,
    PLTR_PING = 10,
    PLTR_PONG = 11,
    PLTR_GOODBYE = 12,
    PLTR_INPUT_OBSERVE = 13,
    PLTR_INPUT_SAMPLE = 14,
    PLTR_CLIENT_FRAME_BATCH = 15,
    PLTR_OPEN = 16,
    PLTR_NOISE = 17,
    PLTR_PAIR_START = 32,
    PLTR_PAIR_RESPONSE = 33,
    PLTR_PAIR_CONFIRM = 34,
    PLTR_PAIR_RESULT = 35,
    PLTR_PAIR_APPROVAL = 36,
} PltrType;

typedef enum PltrDirection {
    PLTR_CLIENT_TO_RELAY = 1,
    PLTR_RELAY_TO_CLIENT = 2,
} PltrDirection;

typedef enum PltrPhase {
    PLTR_PRE_AUTH = 1,
    PLTR_SECURE = 2,
} PltrPhase;

typedef struct PltrFrame {
    uint16_t type;
    uint32_t sequence;
    const uint8_t *payload;
    uint32_t payload_size;
} PltrFrame;

typedef struct PltrRecordReader {
    uint8_t bytes[2 + PLTR_MAX_RECORD_BODY_SIZE];
    size_t filled;
    size_t body_size;
    PltrPhase phase;
} PltrRecordReader;

// No allocation or unbounded queue. The returned body view is valid until the
// next push. Call push repeatedly for coalesced TCP records, advancing by
// consumed. A negative result closes the link and requires reinitialization.
void pltr_record_reader_init(PltrRecordReader *reader, PltrPhase phase);
int pltr_record_reader_set_phase(PltrRecordReader *reader, PltrPhase phase);
int pltr_record_reader_push(PltrRecordReader *reader, const uint8_t *bytes,
                            size_t size, size_t *consumed,
                            const uint8_t **body, size_t *body_size);

// Decode a complete PLTR frame. The caller owns the buffer and must keep it
// alive while using the payload view. expected_sequence starts at 1.
int pltr_decode_frame(const uint8_t *bytes, size_t size,
                      PltrDirection direction, PltrPhase phase,
                      uint32_t expected_sequence, PltrFrame *out);

// Encode a validated frame. The sequence must start at 1 and never wrap.
int pltr_encode_frame(uint16_t type, uint32_t sequence,
                      const uint8_t *payload, size_t payload_size,
                      PltrDirection direction, PltrPhase phase,
                      uint8_t *out, size_t capacity, size_t *written);

// Decode a complete length-prefixed plaintext record before the Noise
// handshake. For encrypted records, decrypt first and pass the resulting
// plaintext PLTR frame to pltr_decode_frame instead.
int pltr_decode_record(const uint8_t *bytes, size_t size,
                       PltrDirection direction, PltrPhase phase,
                       uint32_t expected_sequence, PltrFrame *out);

#ifdef __cplusplus
}
#endif

#endif
