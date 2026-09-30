#include "ble_lab.h"
#include "ble_lab_internal.h"
#include "coc_stream.hpp"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <sys/socket.h>

extern "C" int pltr_ble_lab_run_coc_session(PltrBleLab *lab, int coc_fd,
                                              int stop_fd) {
    PltrIdentityStore *store = pltr_ble_lab_identity_store(lab);
    if (!store || coc_fd < 0) return -1;
    l2cap_options options{};
    socklen_t length = sizeof(options);
    if (getsockopt(coc_fd, SOL_L2CAP, L2CAP_OPTIONS, &options, &length) != 0) {
        std::fprintf(stderr, "Bluetooth data channel options unavailable (errno %d).\n", errno);
        return -1;
    }
    if (length < sizeof(options)) {
        std::fprintf(stderr, "Bluetooth data channel options were truncated.\n");
        return -1;
    }
    std::fprintf(stderr, "Bluetooth data channel outgoing MTU: %u.\n", options.omtu);
    return pltr_run_coc_stream_session(coc_fd, *store, stop_fd, options.omtu);
}
