#include "client_link.h"
#include "link.h"

#include <assert.h>
#include <sodium.h>
#include <stdint.h>
#include <string.h>

static int approve(void *context, const uint8_t public_key[32]) {
    return memcmp(context, public_key, 32) == 0;
}

static size_t to_relay(PltrLink *relay, const uint8_t *bytes, size_t size,
                       uint8_t *reply, size_t capacity, uint16_t *type) {
    size_t offset = 0, total = 0;
    *type = 0;
    while (offset < size) {
        size_t consumed = 0, written = 0;
        PltrFrame frame;
        assert(pltr_link_receive(relay, bytes + offset, size - offset,
                                 &consumed, reply + total, capacity - total,
                                 &written, &frame) == 1);
        assert(consumed != 0);
        offset += consumed;
        total += written;
        if (frame.type) *type = frame.type;
    }
    return total;
}

static size_t to_client(PltrClientLink *client, const uint8_t *bytes,
                        size_t size, uint8_t *reply, size_t capacity,
                        uint16_t *type, uint8_t *payload,
                        size_t payload_capacity, size_t *payload_size) {
    size_t offset = 0, total = 0;
    *type = 0;
    while (offset < size) {
        size_t consumed = 0, written = 0;
        assert(pltr_client_link_receive(client, bytes + offset, size - offset,
                                        &consumed, reply + total, capacity - total,
                                        &written, type, payload,
                                        payload_capacity, payload_size) == 1);
        assert(consumed != 0);
        offset += consumed;
        total += written;
    }
    return total;
}

int main(int argc, char **argv) {
    (void)argv;
    const uint8_t link_type = argc > 1 ? 1 : 2;
    assert(sodium_init() >= 0);
    uint8_t client_public[32], client_private[32];
    uint8_t relay_public[32], relay_private[32];
    crypto_box_keypair(client_public, client_private);
    crypto_box_keypair(relay_public, relay_private);
    PltrClientLink *client = pltr_client_link_create(client_private,
                                                     relay_public, link_type);
    assert(client != NULL);
    assert(pltr_client_link_peer_version(client) == NULL);
    PltrLink relay;
    assert(pltr_link_init(&relay, PLTR_NOISE_RESPONDER, relay_private,
                          NULL, approve, client_public, link_type) == 0);
    uint8_t start[256], relay_reply[256], client_reply[256];
    uint8_t unused[256], payload[64];
    size_t start_size, relay_size, client_size, unused_size, payload_size;
    uint16_t type;
    assert(pltr_client_link_start(client, start, sizeof(start),
                                  &start_size) == 0);
    relay_size = to_relay(&relay, start, start_size, relay_reply,
                           sizeof(relay_reply), &type);
    assert(type == 0 && relay_size != 0);
    client_size = to_client(client, relay_reply, relay_size,
                            client_reply, sizeof(client_reply), &type,
                            payload, sizeof(payload), &payload_size);
    assert(type == 0 && client_size != 0);
    unused_size = to_relay(&relay, client_reply, client_size, unused,
                            sizeof(unused), &type);
    assert(type == 0 && unused_size == 0);
    assert(strcmp(pltr_client_link_peer_version(client), "0.1.1") == 0);

    const uint8_t status[] = {1, 0x6a, 0x05, 0x57, 0x03, 1, 1, 0};
    assert(pltr_link_send(&relay, PLTR_STATUS, status, sizeof(status),
                          relay_reply, sizeof(relay_reply), &relay_size) == 0);
    client_size = to_client(client, relay_reply, relay_size,
                            client_reply, sizeof(client_reply), &type,
                            payload, sizeof(payload), &payload_size);
    assert(type == PLTR_STATUS && client_size == 0 &&
           payload_size == sizeof(status) &&
           memcmp(payload, status, sizeof(status)) == 0);

    const uint8_t ready[] = {0x24, 0, 0, 0, 0};
    assert(pltr_client_link_send(client, PLTR_SESSION_READY, ready,
                                 sizeof(ready), start, sizeof(start),
                                 &start_size) == 0);
    unused_size = to_relay(&relay, start, start_size, unused,
                            sizeof(unused), &type);
    assert(type == PLTR_SESSION_READY && unused_size == 0 &&
           relay.relay_session.stage == PLTR_RELAY_READY);
    pltr_client_link_destroy(client);
    pltr_link_clear(&relay);
    return 0;
}
