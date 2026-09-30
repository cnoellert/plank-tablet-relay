#ifndef PLANK_RELAY_DNSSD_HPP
#define PLANK_RELAY_DNSSD_HPP

#include <cstdint>
#include <string>

#ifdef PLTR_HAS_AVAHI
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/thread-watch.h>
#endif

// Discovery is advisory. Pairing and sessions authenticate the pinned Relay
// identity independently of DNS-SD names and TXT records.
class PltrDnsSd {
public:
    PltrDnsSd(const std::uint8_t public_key[32], std::uint16_t port);
    ~PltrDnsSd();
    PltrDnsSd(const PltrDnsSd &) = delete;
    PltrDnsSd &operator=(const PltrDnsSd &) = delete;
    bool start();
    void stop();
    void setPairing(bool open);
    bool pairing() const { return pairing_; }

private:
    std::string name_;
    std::string rid_;
    std::uint16_t port_;
    bool pairing_ = false;
#ifdef PLTR_HAS_AVAHI
    AvahiThreadedPoll *poll_ = nullptr;
    AvahiClient *client_ = nullptr;
    AvahiEntryGroup *group_ = nullptr;
    bool running_ = false;
    void publish(AvahiClient *client);
    static void clientChanged(AvahiClient *client, AvahiClientState state,
                              void *context);
    static void groupChanged(AvahiEntryGroup *group,
                             AvahiEntryGroupState state, void *context);
#endif
};

#endif
