#include "link.h"

#include <string.h>
#include <sodium.h>

static void le16(uint8_t *out, size_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void le32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

static int fail(PltrLink *link) {
    pltr_noise_clear(&link->noise);
    sodium_memzero(link->plaintext, sizeof(link->plaintext));
    link->stage = PLTR_LINK_FAILED;
    return -1;
}

static int pack_record(uint8_t *out, size_t capacity,
                       const uint8_t *body, size_t body_size,
                       size_t *written) {
    if (out == NULL || written == NULL || body == NULL ||
        body_size > UINT16_MAX || capacity < body_size + 2) return -1;
    le16(out, body_size);
    memcpy(out + 2, body, body_size);
    *written = body_size + 2;
    return 0;
}

static int send_plain(PltrLink *link, uint16_t type, const uint8_t *payload,
                      size_t payload_size, uint8_t *out, size_t capacity,
                      size_t *written) {
    uint8_t frame[PLTR_MAX_FRAME_SIZE];
    size_t frame_size;
    const PltrDirection direction = link->role == PLTR_NOISE_INITIATOR ?
                                    PLTR_CLIENT_TO_RELAY : PLTR_RELAY_TO_CLIENT;
    if (pltr_encode_frame(type, link->outgoing_sequence, payload,
                          payload_size, direction, PLTR_PRE_AUTH,
                          frame, sizeof(frame), &frame_size) != 0 ||
        pack_record(out, capacity, frame, frame_size, written) != 0) return -1;
    ++link->outgoing_sequence;
    return 0;
}

static int send_secure(PltrLink *link, uint16_t type, const uint8_t *payload,
                       size_t payload_size, uint8_t *out, size_t capacity,
                       size_t *written) {
    uint8_t frame[PLTR_MAX_FRAME_SIZE];
    size_t frame_size, cipher_size;
    const PltrDirection direction = link->role == PLTR_NOISE_INITIATOR ?
                                    PLTR_CLIENT_TO_RELAY : PLTR_RELAY_TO_CLIENT;
    if (out == NULL || payload_size > PLTR_MAX_PAYLOAD_SIZE ||
        capacity < PLTR_HEADER_SIZE + payload_size + 18 ||
        pltr_encode_frame(type, link->outgoing_sequence, payload,
                          payload_size, direction, PLTR_SECURE,
                          frame, sizeof(frame), &frame_size) != 0 ||
        pltr_noise_encrypt(&link->noise, frame, frame_size,
                           out + 2, capacity - 2, &cipher_size) != 0) return -1;
    le16(out, cipher_size);
    *written = cipher_size + 2;
    ++link->outgoing_sequence;
    return 0;
}

static int send_hello(PltrLink *link, uint8_t *out, size_t capacity,
                      size_t *written) {
    uint8_t hello[18] = {0};
    le16(hello, PLTR_VERSION);
    le16(hello + 2, PLTR_VERSION);
    hello[4] = link->role == PLTR_NOISE_INITIATOR ? 2 : 1;
    le32(hello + 8, link->local_features);
    hello[12] = 5;
    memcpy(hello + 13, "0.1.1", 5);
    return send_secure(link, PLTR_HELLO, hello, sizeof(hello),
                       out, capacity, written);
}

int pltr_link_init(PltrLink *link, PltrNoiseRole role,
                   const uint8_t private_key[32],
                   const uint8_t relay_public_key[32],
                   PltrApproveClient approve_client, void *approve_context,
                   uint8_t link_type) {
    if (link == NULL || private_key == NULL ||
        (role == PLTR_NOISE_INITIATOR && relay_public_key == NULL) ||
        (role == PLTR_NOISE_RESPONDER && approve_client == NULL) ||
        (role != PLTR_NOISE_INITIATOR && role != PLTR_NOISE_RESPONDER)) return -1;
    memset(link, 0, sizeof(*link));
    if (pltr_noise_init(&link->noise, role, private_key,
                        relay_public_key, link_type) != 0) return fail(link);
    pltr_record_reader_init(&link->reader, PLTR_PRE_AUTH);
    link->role = role;
    link->stage = role == PLTR_NOISE_INITIATOR ?
                  PLTR_LINK_WAIT_SECOND : PLTR_LINK_WAIT_OPEN;
    link->approve_client = approve_client;
    link->approve_context = approve_context;
    link->incoming_sequence = link->outgoing_sequence = 1;
    link->local_features = PLTR_FEATURE_RAW_HID;
    return 0;
}

int pltr_link_enable_input_observer(PltrLink *link) {
    if (link == NULL || link->incoming_sequence != 1 ||
        link->outgoing_sequence != 1 ||
        (link->stage != PLTR_LINK_WAIT_OPEN &&
         link->stage != PLTR_LINK_WAIT_SECOND)) return -1;
    link->local_features |= PLTR_FEATURE_INPUT_OBSERVER;
    return 0;
}

static int observer_enabled(const PltrLink *link) {
    return (link->local_features & link->peer_features &
            PLTR_FEATURE_INPUT_OBSERVER) != 0;
}

void pltr_link_clear(PltrLink *link) {
    if (link) sodium_memzero(link, sizeof(*link));
}

int pltr_link_start(PltrLink *link, uint8_t *out, size_t capacity,
                    size_t *written) {
    if (link == NULL || out == NULL || written == NULL ||
        link->role != PLTR_NOISE_INITIATOR ||
        link->stage != PLTR_LINK_WAIT_SECOND ||
        link->outgoing_sequence != 1) return -1;
    uint8_t open[] = {1}, message[96];
    size_t first_size, message_size, second_size;
    if (send_plain(link, PLTR_OPEN, open, sizeof(open),
                   out, capacity, &first_size) != 0 ||
        pltr_noise_write_first(&link->noise, NULL, 0,
                               message, sizeof(message), &message_size) != 0 ||
        send_plain(link, PLTR_NOISE, message, message_size,
                   out + first_size, capacity - first_size,
                   &second_size) != 0) return fail(link);
    *written = first_size + second_size;
    return 0;
}

int pltr_link_receive(PltrLink *link, const uint8_t *bytes, size_t size,
                      size_t *consumed, uint8_t *reply, size_t reply_capacity,
                      size_t *reply_size, PltrFrame *frame) {
    if (link == NULL || consumed == NULL || reply_size == NULL ||
        frame == NULL || (reply == NULL && reply_capacity != 0) ||
        link->stage == PLTR_LINK_FAILED ||
        link->stage == PLTR_LINK_CLOSED) return -1;
    *reply_size = 0;
    memset(frame, 0, sizeof(*frame));
    const uint8_t *body;
    size_t body_size;
    const int record = pltr_record_reader_push(&link->reader, bytes, size,
                                                consumed, &body, &body_size);
    if (record < 0) return fail(link);
    if (record == 0) return 0;

    size_t plaintext_size;
    if (link->reader.phase == PLTR_SECURE) {
        if (pltr_noise_decrypt(&link->noise, body, body_size,
                               link->plaintext, sizeof(link->plaintext),
                               &plaintext_size) != 0) return fail(link);
        body = link->plaintext;
        body_size = plaintext_size;
    }
    const PltrDirection direction = link->role == PLTR_NOISE_INITIATOR ?
                                    PLTR_RELAY_TO_CLIENT : PLTR_CLIENT_TO_RELAY;
    if (pltr_decode_frame(body, body_size, direction, link->reader.phase,
                          link->incoming_sequence, frame) != 0) return fail(link);

    if (link->stage == PLTR_LINK_WAIT_OPEN) {
        if (frame->type != PLTR_OPEN || frame->payload[0] != 1)
            return fail(link);
        link->stage = PLTR_LINK_WAIT_FIRST;
    } else if (link->stage == PLTR_LINK_WAIT_FIRST) {
        uint8_t client_key[32], unused[1], message[48];
        size_t read_size, message_size, first_size, hello_size;
        if (frame->type != PLTR_NOISE || frame->payload_size != 96 ||
            pltr_noise_read_first(&link->noise, frame->payload,
                                   frame->payload_size, client_key,
                                   unused, sizeof(unused), &read_size) != 0 ||
            read_size != 0 ||
            link->approve_client(link->approve_context, client_key) != 1 ||
            pltr_noise_write_second(&link->noise, client_key, NULL, 0,
                                     message, sizeof(message), &message_size) != 0 ||
            send_plain(link, PLTR_NOISE, message, message_size,
                       reply, reply_capacity, &first_size) != 0 ||
            pltr_record_reader_set_phase(&link->reader, PLTR_SECURE) != 0 ||
            send_hello(link, reply + first_size, reply_capacity - first_size,
                       &hello_size) != 0) {
            sodium_memzero(client_key, sizeof(client_key));
            return fail(link);
        }
        sodium_memzero(client_key, sizeof(client_key));
        *reply_size = first_size + hello_size;
        pltr_relay_session_init(&link->relay_session,
                                 link->incoming_sequence + 1);
        link->stage = PLTR_LINK_WAIT_HELLO;
    } else if (link->stage == PLTR_LINK_WAIT_SECOND) {
        uint8_t unused[1];
        size_t read_size;
        if (frame->type != PLTR_NOISE || frame->payload_size != 48 ||
            pltr_noise_read_second(&link->noise, frame->payload,
                                    frame->payload_size, unused,
                                    sizeof(unused), &read_size) != 0 ||
            read_size != 0 ||
            pltr_record_reader_set_phase(&link->reader, PLTR_SECURE) != 0 ||
            send_hello(link, reply, reply_capacity, reply_size) != 0)
            return fail(link);
        link->stage = PLTR_LINK_WAIT_HELLO;
    } else if (link->stage == PLTR_LINK_WAIT_HELLO) {
        if (frame->type != PLTR_HELLO) return fail(link);
        link->peer_features = (uint32_t)frame->payload[8] |
            ((uint32_t)frame->payload[9] << 8) |
            ((uint32_t)frame->payload[10] << 16) |
            ((uint32_t)frame->payload[11] << 24);
        if (link->role == PLTR_NOISE_RESPONDER &&
            pltr_relay_session_accept(&link->relay_session, body,
                                       body_size, frame) != 0) return fail(link);
        const size_t version_size = frame->payload[12];
        memcpy(link->peer_version, frame->payload + 13, version_size);
        link->peer_version[version_size] = '\0';
        link->stage = PLTR_LINK_READY;
    } else if (link->stage == PLTR_LINK_READY) {
        if ((frame->type == PLTR_INPUT_OBSERVE ||
             frame->type == PLTR_INPUT_SAMPLE) && !observer_enabled(link))
            return fail(link);
        if (frame->type == PLTR_HELLO ||
            (link->role == PLTR_NOISE_RESPONDER &&
             pltr_relay_session_accept(&link->relay_session, body,
                                        body_size, frame) != 0)) return fail(link);
    } else {
        return fail(link);
    }
    if (link->incoming_sequence == UINT32_MAX - 1) return fail(link);
    ++link->incoming_sequence;
    if (frame->type == PLTR_GOODBYE || frame->type == PLTR_SESSION_END)
        link->stage = PLTR_LINK_CLOSED;
    if (frame->type == PLTR_OPEN || frame->type == PLTR_NOISE ||
        frame->type == PLTR_HELLO) memset(frame, 0, sizeof(*frame));
    return 1;
}

int pltr_link_send(PltrLink *link, uint16_t type,
                   const uint8_t *payload, size_t payload_size,
                   uint8_t *out, size_t capacity, size_t *written) {
    if (link == NULL || link->stage != PLTR_LINK_READY ||
        ((type == PLTR_INPUT_OBSERVE || type == PLTR_INPUT_SAMPLE) &&
         !observer_enabled(link)) ||
        (link->role == PLTR_NOISE_RESPONDER && type == PLTR_INPUT_SAMPLE &&
         link->relay_session.stage != PLTR_RELAY_OBSERVING) ||
        (link->role == PLTR_NOISE_RESPONDER &&
         (type == PLTR_CLIENT_FRAME || type == PLTR_CLIENT_FRAME_BATCH) &&
         link->relay_session.stage != PLTR_RELAY_READY)) return -1;
    if (send_secure(link, type, payload, payload_size,
                    out, capacity, written) != 0) return fail(link);
    if (type == PLTR_GOODBYE || type == PLTR_SESSION_END)
        link->stage = PLTR_LINK_CLOSED;
    return 0;
}
