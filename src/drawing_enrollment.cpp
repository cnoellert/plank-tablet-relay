// SPDX-License-Identifier: GPL-3.0-or-later
#include "drawing_enrollment.hpp"
#include "noise.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sodium.h>

namespace {
std::uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool transfer(int fd, std::uint8_t *bytes, std::size_t size, bool sending,
    std::uint64_t deadline, PltrDrawingEnrollment &grants, int stop) {
    while (size) {
        const auto now = now_ms();
        grants.service(now);
        if (now >= deadline) return false;
        pollfd p[2] = {{fd, static_cast<short>(sending ? POLLOUT : POLLIN),0}, {stop,POLLIN,0}};
        const int r = poll(p, stop >= 0 ? 2 : 1, static_cast<int>(std::min<std::uint64_t>(10, deadline-now)));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 || (stop >= 0 && p[1].revents) || (p[0].revents & (POLLERR|POLLNVAL))) return false;
        if (!(p[0].revents & (sending ? POLLOUT : POLLIN))) {
            if (p[0].revents & POLLHUP) return false;
            continue;
        }
        const auto n = sending ? send(fd,bytes,size,MSG_DONTWAIT|MSG_NOSIGNAL) : recv(fd,bytes,size,MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return false;
        bytes += n; size -= static_cast<std::size_t>(n);
    }
    return true;
}
}
PltrDrawingEnrollment::~PltrDrawingEnrollment() {
    for (auto &c : connections_) drop(c);
    sodium_memzero(grants_.data(), sizeof(grants_));
}
void PltrDrawingEnrollment::drop(Connection &c) {
    if (c.fd >= 0) ::close(c.fd);
    c = Connection{};
}
PltrDrawingEnrollment::Grant *PltrDrawingEnrollment::find(const std::uint8_t id[16]) {
    for (auto &g : grants_) if (g.state && sodium_memcmp(g.id.data(), id, 16) == 0) return &g;
    return nullptr;
}
std::uint8_t PltrDrawingEnrollment::command(const std::uint8_t *r, std::uint64_t now) {
    if (std::memcmp(r,"PLEN\1",5) || (r[5] != 1 && r[5] != 2) ||
        sodium_is_zero(r+6,16) || sodium_is_zero(r+22,32)) return 1;
    if (sodium_memcmp(r+54,store_.public_key,32)) return 2;
    for (auto &g : grants_) if (g.state && now >= g.deadline) g = Grant{};
    auto *g = find(r+6);
    if (r[5] == 2) {
        if (!g || sodium_memcmp(g->client.data(),r+22,32)) return 3;
        g->state = 3; return 0;
    }
    // Request ids are never idempotent authorization: a repeated request,
    // including a canceled or failed attempt, must receive a refusal.
    if (g) return 3;
    std::uint8_t shared[32];
    const int valid = crypto_scalarmult(shared,store_.private_key,r+22);
    sodium_memzero(shared,sizeof(shared));
    if (valid != 0) return 1;
    for (auto &slot : grants_) if (!slot.state) {
        std::memcpy(slot.id.data(),r+6,16); std::memcpy(slot.client.data(),r+22,32);
        slot.deadline = now + GrantLifetimeMs; slot.state = 1; return 0;
    }
    return 4;
}
void PltrDrawingEnrollment::service(std::uint64_t now) {
    for (auto &c : connections_) {
        if (c.fd < 0) {
            c.fd = listener_.accept();
            if (c.fd < 0) continue;
            c.deadline = now + 250;
        }
        if (now >= c.deadline) { drop(c); continue; }
        if (!c.replying) {
            const auto n = recv(c.fd,c.request.data()+c.received,c.request.size()-c.received,MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n <= 0) { drop(c); continue; }
            c.received += static_cast<std::size_t>(n);
            if (c.received > 86) { drop(c); continue; }
            if (c.received < 86) continue;
            std::memcpy(c.reply.data(),"PLEN\1",5);
            c.reply[5] = command(c.request.data(),now);
            std::memcpy(c.reply.data()+6,c.request.data()+6,16); c.replying = true;
        }
        const auto n = send(c.fd,c.reply.data()+c.sent,c.reply.size()-c.sent,MSG_DONTWAIT|MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) { drop(c); continue; }
        c.sent += static_cast<std::size_t>(n);
        if (c.sent == c.reply.size()) drop(c);
    }
}
bool PltrDrawingEnrollment::claim(const std::uint8_t id[16], const std::uint8_t client[32], std::uint64_t now) {
    auto *g = find(id);
    if (!g || g->state != 1 || now >= g->deadline || sodium_memcmp(g->client.data(),client,32)) return false;
    g->state = 2; return true;
}
bool PltrDrawingEnrollment::commit(const std::uint8_t id[16], const std::uint8_t client[32], std::uint64_t now) {
    auto *g = find(id);
    if (!g || g->state != 2 || now >= g->deadline || sodium_memcmp(g->client.data(),client,32)) return false;
    g->state = 3;
    return pltr_identity_store_add(&store_,client) == 0;
}
int pltr_run_enrollment(int fd, PltrIdentityStore &store, PltrDrawingEnrollment &grants, int stop) {
    PltrNoise noise{};
    if (pltr_noise_init_enrollment(&noise,PLTR_NOISE_RESPONDER,store.private_key,nullptr)) return -1;
    // One deadline for the entire proof, including partial reads and writes.
    const auto deadline = now_ms()+10000;
    std::uint8_t input[119], client[32], id[16], plain[21], reply[50];
    std::size_t n;
    int result = -1;
    if (transfer(fd,input,sizeof(input),false,deadline,grants,stop) &&
        std::memcmp(input,"PLEN\1",5) == 0 && input[5] == 112 && input[6] == 0 &&
        pltr_noise_read_first(&noise,input+7,112,client,id,sizeof(id),&n) == 0 && n == 16 &&
        grants.claim(id,client,now_ms()) &&
        pltr_noise_write_second(&noise,client,nullptr,0,reply+2,48,&n) == 0 && n == 48) {
        reply[0] = 48; reply[1] = 0;
        if (transfer(fd,reply,50,true,deadline,grants,stop) &&
            transfer(fd,input,39,false,deadline,grants,stop) && input[0] == 37 && input[1] == 0 &&
            pltr_noise_decrypt(&noise,input+2,37,plain,sizeof(plain),&n) == 0 && n == 21 &&
            std::memcmp(plain,"PLEN\1",5) == 0 && sodium_memcmp(plain+5,id,16) == 0) {
            grants.service(now_ms());
            if (grants.commit(id,client,now_ms()) &&
                pltr_noise_encrypt(&noise,plain,21,reply+2,sizeof(reply)-2,&n) == 0 && n == 37) {
                reply[0] = 37; reply[1] = 0;
                if (transfer(fd,reply,39,true,deadline,grants,stop)) result = 0;
            }
        }
    }
    pltr_noise_clear(&noise); sodium_memzero(client,sizeof(client));
    sodium_memzero(input,sizeof(input)); return result;
}
