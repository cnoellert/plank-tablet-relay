#include "dnssd.hpp"

#include <cstdio>
#include <sodium.h>

#ifdef PLTR_HAS_AVAHI
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#endif

PltrDnsSd::PltrDnsSd(const std::uint8_t public_key[32], std::uint16_t port)
    : port_(port) {
    std::uint8_t fingerprint[32]{};
    if (crypto_generichash(fingerprint, sizeof(fingerprint), public_key, 32,
                           nullptr, 0) != 0) return;
    char rid[17]{};
    for (unsigned index = 0; index < 8; ++index)
        std::snprintf(rid + index * 2, 3, "%02x", fingerprint[index]);
    rid_ = rid;
    name_ = "PLANK Wacom Relay " + rid_;
}

PltrDnsSd::~PltrDnsSd() { stop(); }

#ifdef PLTR_HAS_AVAHI
void PltrDnsSd::publish(AvahiClient *client) {
    if (group_ == nullptr)
        group_ = avahi_entry_group_new(client, groupChanged, this);
    if (group_ == nullptr) return;
    if (avahi_entry_group_is_empty(group_) == 0) return;
    const char *state = pairing_ ? "pair=1" : "pair=0";
    const std::string rid_txt = "rid=" + rid_;
    const int added = avahi_entry_group_add_service(
        group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, AvahiPublishFlags(0),
        name_.c_str(), "_plank-tablet._tcp", nullptr, nullptr, port_,
        "v=1", rid_txt.c_str(), state, nullptr);
    if (added == AVAHI_ERR_COLLISION) {
        char *alternate = avahi_alternative_service_name(name_.c_str());
        if (alternate != nullptr) {
            name_ = alternate;
            avahi_free(alternate);
            avahi_entry_group_reset(group_);
            publish(client);
        }
    } else if (added >= 0) {
        (void)avahi_entry_group_commit(group_);
    }
}

void PltrDnsSd::clientChanged(AvahiClient *client, AvahiClientState state,
                              void *context) {
    auto *self = static_cast<PltrDnsSd *>(context);
    if (state == AVAHI_CLIENT_S_RUNNING) self->publish(client);
    else if ((state == AVAHI_CLIENT_S_COLLISION ||
              state == AVAHI_CLIENT_S_REGISTERING) && self->group_ != nullptr)
        (void)avahi_entry_group_reset(self->group_);
}

void PltrDnsSd::groupChanged(AvahiEntryGroup *group,
                             AvahiEntryGroupState state, void *context) {
    auto *self = static_cast<PltrDnsSd *>(context);
    if (state == AVAHI_ENTRY_GROUP_COLLISION) {
        char *alternate = avahi_alternative_service_name(self->name_.c_str());
        if (alternate != nullptr) {
            self->name_ = alternate;
            avahi_free(alternate);
            (void)avahi_entry_group_reset(group);
            self->publish(avahi_entry_group_get_client(group));
        }
    }
}

bool PltrDnsSd::start() {
    if (rid_.empty()) return false;
    poll_ = avahi_threaded_poll_new();
    if (poll_ == nullptr) return false;
    int error = 0;
    client_ = avahi_client_new(avahi_threaded_poll_get(poll_),
                               AvahiClientFlags(0), clientChanged, this, &error);
    if (client_ == nullptr || avahi_threaded_poll_start(poll_) != 0) {
        stop();
        return false;
    }
    running_ = true;
    return true;
}

void PltrDnsSd::stop() {
    if (poll_ == nullptr) return;
    if (running_) avahi_threaded_poll_stop(poll_);
    running_ = false;
    if (client_ != nullptr) avahi_client_free(client_);
    client_ = nullptr;
    group_ = nullptr;
    avahi_threaded_poll_free(poll_);
    poll_ = nullptr;
}

void PltrDnsSd::setPairing(bool open) {
    if (pairing_ == open) return;
    if (poll_ != nullptr && running_) avahi_threaded_poll_lock(poll_);
    pairing_ = open;
    if (group_ != nullptr &&
        avahi_entry_group_get_state(group_) == AVAHI_ENTRY_GROUP_ESTABLISHED) {
        const std::string rid_txt = "rid=" + rid_;
        (void)avahi_entry_group_update_service_txt(
            group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET,
            AvahiPublishFlags(0), name_.c_str(), "_plank-tablet._tcp",
            nullptr, "v=1", rid_txt.c_str(), open ? "pair=1" : "pair=0",
            nullptr);
    }
    if (poll_ != nullptr && running_) avahi_threaded_poll_unlock(poll_);
}
#else
bool PltrDnsSd::start() { return false; }
void PltrDnsSd::stop() {}
void PltrDnsSd::setPairing(bool open) { pairing_ = open; }
#endif
