#include "coc_stream.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
ssize_t readable(int fd, std::uint8_t *bytes, std::size_t capacity) {
    pollfd item{fd, POLLIN, 0};
    assert(poll(&item, 1, 3000) == 1);
    return recv(fd, bytes, capacity, 0);
}
}

void run_case(std::size_t outgoing_mtu) {
    int packets[2], stream[2], stop[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, packets) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
    assert(pipe(stop) == 0);
    std::thread bridge([&] {
        assert(pltr_bridge_coc_stream(packets[0], stream[0], stop[0], outgoing_mtu) == 0);
    });
    std::array<std::uint8_t, 5000> outbound{};
    for (std::size_t i = 0; i < outbound.size(); ++i)
        outbound[i] = static_cast<std::uint8_t>(i);
    assert(send(stream[1], outbound.data(), outbound.size(), 0) ==
           static_cast<ssize_t>(outbound.size()));
    std::vector<std::uint8_t> reassembled;
    while (reassembled.size() < outbound.size()) {
        std::array<std::uint8_t, 4096> packet{};
        const ssize_t count = readable(packets[1], packet.data(), packet.size());
        assert(count > 0 && static_cast<std::size_t>(count) <=
               std::min(outgoing_mtu, std::size_t{4096}));
        reassembled.insert(reassembled.end(), packet.begin(), packet.begin() + count);
    }
    assert(std::memcmp(reassembled.data(), outbound.data(), outbound.size()) == 0);

    const std::array<std::uint8_t, 4> first = {8, 7, 6, 5}, second = {4, 3, 2, 1};
    assert(send(packets[1], first.data(), first.size(), 0) == 4);
    assert(send(packets[1], second.data(), second.size(), 0) == 4);
    std::array<std::uint8_t, 8> incoming{};
    std::size_t received = 0;
    while (received < incoming.size()) {
        const ssize_t count = readable(stream[1], incoming.data() + received,
                                       incoming.size() - received);
        assert(count > 0);
        received += static_cast<std::size_t>(count);
    }
    assert(std::memcmp(incoming.data(), first.data(), first.size()) == 0);
    assert(std::memcmp(incoming.data() + 4, second.data(), second.size()) == 0);

    assert(write(stop[1], "x", 1) == 1);
    bridge.join();
    for (int fd : {packets[0], packets[1], stream[0], stream[1], stop[0], stop[1]})
        close(fd);
}

int main() {
    run_case(37);
    run_case(65535);
}
