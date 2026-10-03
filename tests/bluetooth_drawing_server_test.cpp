#include "bluetooth_drawing_server.hpp"
#include <cassert>
int main() {
    const std::string name = "plank-ble-bridge-test-" + std::to_string(getpid());
    auto connect_peer = [&](const std::string &n) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0); assert(fd >= 0);
        sockaddr_un a{}; a.sun_family = AF_UNIX;
        memcpy(a.sun_path + 1, n.data(), n.size());
        assert(connect(fd, reinterpret_cast<sockaddr *>(&a),
                       offsetof(sockaddr_un, sun_path) + 1 + n.size()) == 0);
        return fd;
    };
    PltrBluetoothDrawingServer production;
    // Tests never bind the production name; its production policy has no uid override.
    PltrBluetoothDrawingServer server({name, static_cast<std::uint32_t>(getuid())});
    assert(server.bind());
    PltrBluetoothDrawingServer collision({name, static_cast<std::uint32_t>(getuid())});
    assert(!collision.bind());
    int client = connect_peer(name), accepted = server.accept();
    assert(accepted >= 0); close(accepted); close(client);
    PltrBluetoothDrawingServer denied({name + "-denied", static_cast<std::uint32_t>(getuid() + 1)});
    assert(denied.bind()); client = connect_peer(name + "-denied");
    assert(denied.accept() == -1 && errno == EACCES);
    char byte; assert(recv(client, &byte, 1, 0) == 0); close(client);
}
