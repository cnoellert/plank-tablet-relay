#include "tcp_session.hpp"
#include "link.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sodium.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static void send_bytes(int fd, const std::uint8_t *bytes, std::size_t size) {
    while (size) {
        const ssize_t sent = send(fd, bytes, size, 0);
        assert(sent > 0);
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
    }
}

static std::size_t receive_record(int fd, std::uint8_t *out,
                                  std::size_t capacity) {
    assert(recv(fd, out, 2, MSG_WAITALL) == 2);
    const std::size_t size = 2 + out[0] + (std::size_t(out[1]) << 8);
    assert(size <= capacity);
    assert(recv(fd, out + 2, size - 2, MSG_WAITALL) ==
           static_cast<ssize_t>(size - 2));
    return size;
}

static PltrFrame receive_link(int fd, PltrLink &link,
                              std::uint8_t *reply, std::size_t reply_capacity,
                              std::size_t &reply_size) {
    std::array<std::uint8_t, 2 + PLTR_MAX_RECORD_BODY_SIZE> record{};
    const std::size_t size = receive_record(fd, record.data(), record.size());
    std::size_t consumed = 0;
    PltrFrame frame{};
    assert(pltr_link_receive(&link, record.data(), size, &consumed,
                             reply, reply_capacity, &reply_size, &frame) == 1);
    assert(consumed == size);
    return frame;
}

int main() {
    assert(sodium_init() >= 0);
    const std::string lease_name = "plank-tcp-session-test-" +
                                   std::to_string(getpid());
    char directory[] = "/tmp/pltr-session-XXXXXX";
    assert(mkdtemp(directory) != nullptr);
    PltrIdentityStore store{};
    assert(pltr_identity_store_open(&store, directory) == 0);
    std::uint8_t client_public[32], client_private[32];
    crypto_box_keypair(client_public, client_private);
    assert(pltr_identity_store_add(&store, client_public) == 0);

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int server_result = -2;
    std::thread server([&] {
        server_result = pltr_run_tcp_session(sockets[1], store, -1, lease_name);
        close(sockets[1]);
    });
    PltrLink client{};
    assert(pltr_link_init(&client, PLTR_NOISE_INITIATOR, client_private,
                          store.public_key, nullptr, nullptr, 2) == 0);
    std::array<std::uint8_t, 2 + PLTR_MAX_RECORD_BODY_SIZE> output{}, reply{};
    std::size_t size = 0, reply_size = 0;
    assert(pltr_link_start(&client, output.data(), output.size(), &size) == 0);
    send_bytes(sockets[0], output.data(), size);
    assert(receive_link(sockets[0], client, reply.data(), reply.size(),
                        reply_size).type == 0);
    assert(reply_size != 0); // Client HELLO, encrypted after Noise reply
    const std::size_t hello_size = reply_size;
    assert(receive_link(sockets[0], client, reply.data(), reply.size(),
                        reply_size).type == 0);
    send_bytes(sockets[0], reply.data(), hello_size);
    PltrFrame frame = receive_link(sockets[0], client, reply.data(),
                                    reply.size(), reply_size);
    assert(frame.type == PLTR_STATUS && frame.payload[0] == 0);
    const std::uint8_t ready[] = {0x24, 0, 0, 0, 0};
    assert(pltr_link_send(&client, PLTR_SESSION_READY, ready, sizeof(ready),
                          output.data(), output.size(), &size) == 0);
    send_bytes(sockets[0], output.data(), size);
    const std::uint8_t end[] = {1};
    assert(pltr_link_send(&client, PLTR_SESSION_END, end, sizeof(end),
                          output.data(), output.size(), &size) == 0);
    send_bytes(sockets[0], output.data(), size);
    server.join();
    assert(server_result == 0);
    close(sockets[0]);
    pltr_link_clear(&client);

    // A different service's lease rejects authenticated SESSION_READY and
    // closes this connection without disturbing the owner.
    PltrCaptureLease owner(lease_name), probe(lease_name);
    using Result = PltrCaptureLease::AcquireResult;
    assert(owner.acquire() == Result::Acquired);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    server_result = -2;
    std::thread busy_server([&] {
        server_result = pltr_run_tcp_session(sockets[1], store, -1, lease_name);
        close(sockets[1]);
    });
    assert(pltr_link_init(&client, PLTR_NOISE_INITIATOR, client_private,
                          store.public_key, nullptr, nullptr, 2) == 0);
    assert(pltr_link_start(&client, output.data(), output.size(), &size) == 0);
    send_bytes(sockets[0], output.data(), size);
    assert(receive_link(sockets[0], client, reply.data(), reply.size(),
                        reply_size).type == 0);
    const std::size_t busy_hello_size = reply_size;
    assert(receive_link(sockets[0], client, reply.data(), reply.size(),
                        reply_size).type == 0);
    send_bytes(sockets[0], reply.data(), busy_hello_size);
    frame = receive_link(sockets[0], client, reply.data(),
                         reply.size(), reply_size);
    assert(frame.type == PLTR_STATUS && frame.payload[0] == 0);
    assert(pltr_link_send(&client, PLTR_SESSION_READY, ready, sizeof(ready),
                          output.data(), output.size(), &size) == 0);
    send_bytes(sockets[0], output.data(), size);
    busy_server.join();
    assert(server_result == -1 && owner.held());
    assert(probe.acquire() == Result::Busy);
    close(sockets[0]);
    pltr_link_clear(&client);
    owner.release();
    assert(probe.acquire() == Result::Acquired);
    probe.release();

    // The socket may be accepted, but an unknown static key gets no session.
    assert(pltr_identity_store_remove(&store, client_public) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    server_result = -2;
    std::thread unpaired([&] {
        server_result = pltr_run_tcp_session(sockets[1], store, -1, lease_name);
        close(sockets[1]);
    });
    assert(pltr_link_init(&client, PLTR_NOISE_INITIATOR, client_private,
                          store.public_key, nullptr, nullptr, 2) == 0);
    assert(pltr_link_start(&client, output.data(), output.size(), &size) == 0);
    send_bytes(sockets[0], output.data(), size);
    unpaired.join();
    assert(server_result == -1);
    close(sockets[0]);
    pltr_link_clear(&client);

    pltr_identity_store_close(&store);
    for (const char *name : {"identity.key", "paired-clients.json", "store.lock"}) {
        char path[160];
        const int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
        assert(length > 0 && length < static_cast<int>(sizeof(path)));
        assert(unlink(path) == 0);
    }
    assert(rmdir(directory) == 0);
    return 0;
}
