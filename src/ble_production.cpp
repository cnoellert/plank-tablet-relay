#include "ble_lab.h"
#include "ble_lab_internal.h"
#include "coc_stream.hpp"

#include <bluetooth/bluetooth.h>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <sys/socket.h>

extern "C" int pltr_ble_lab_run_coc_session(PltrBleLab *lab, int coc_fd,
                                              int stop_fd) {
    PltrIdentityStore *store = pltr_ble_lab_identity_store(lab);
    if (!store || coc_fd < 0) return -1;
    // SOL_L2CAP/L2CAP_OPTIONS is a BR/EDR option. On an established LE CoC,
    // Linux rejects it with EINVAL; BT_SNDMTU gives the peer's LE SDU limit.
    std::uint16_t outgoing_mtu = 0;
    socklen_t length = sizeof(outgoing_mtu);
    if (getsockopt(coc_fd, SOL_BLUETOOTH, BT_SNDMTU,
                   &outgoing_mtu, &length) != 0) {
        std::fprintf(stderr, "Bluetooth LE send MTU unavailable (errno %d).\n", errno);
        return -1;
    }
    if (length != sizeof(outgoing_mtu) || outgoing_mtu < 23) {
        std::fprintf(stderr, "Bluetooth LE send MTU was invalid.\n");
        return -1;
    }
    std::fprintf(stderr, "Bluetooth data channel outgoing MTU: %u.\n", outgoing_mtu);
    return pltr_run_coc_stream_session(coc_fd, *store, stop_fd, outgoing_mtu);
}
