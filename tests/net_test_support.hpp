#pragma once

// Shared helpers for the two network test translation units. The transport is only reachable
// through the wrapped API, so both files drive the same listener/connect/accept sequence.

#include "cosmos/cosmos.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

inline void must(bool ok) { assert(ok); }

struct Address {
    struct sockaddr_in raw{};

    Address() {
        raw.sin_family = AF_INET;
        raw.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    explicit Address(uint16_t port) : Address() { raw.sin_port = htons(port); }

    const struct sockaddr* as_sockaddr() const {
        return reinterpret_cast<const struct sockaddr*>(&raw);
    }
    struct sockaddr* as_mutable_sockaddr() { return reinterpret_cast<struct sockaddr*>(&raw); }
    socklen_t size() const { return sizeof(raw); }
};

inline cosmos::FaultConfig network_config(cosmos::SiteId site, cosmos::FaultKind kind,
                                          cosmos::Duration amount = {}, double rate = 1.0) {
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Network);
    must(cfg.activate_site(site));
    cosmos::FaultRule rule;
    rule.rate = rate;
    rule.amount = amount;
    must(rule.outcomes.add(kind, 1.0));
    must(cfg.set_rule(site, rule));
    return cfg;
}

inline int bind_listener(uint16_t* port, bool nonblocking = false) {
    const int fd = socket(AF_INET, SOCK_STREAM | (nonblocking ? SOCK_NONBLOCK : 0), 0);
    must(fd >= 0);
    Address addr(0);
    must(bind(fd, addr.as_sockaddr(), addr.size()) == 0);
    must(listen(fd, 4) == 0);
    socklen_t len = addr.size();
    must(getsockname(fd, addr.as_mutable_sockaddr(), &len) == 0);
    *port = ntohs(addr.raw.sin_port);
    return fd;
}

inline int connect_to(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    must(fd >= 0);
    const Address addr(port);
    must(connect(fd, addr.as_sockaddr(), addr.size()) == 0);
    return fd;
}

struct Cluster {
    int listener = -1;
    int client = -1;
    int server = -1;
};

inline Cluster open_pair(bool listener_nonblocking = false) {
    Cluster out;
    uint16_t port = 0;
    out.listener = bind_listener(&port, listener_nonblocking);
    out.client = connect_to(port);
    Address peer;
    socklen_t len = peer.size();
    out.server = accept(out.listener, peer.as_mutable_sockaddr(), &len);
    must(out.server >= 0);
    return out;
}

inline void drop_pair(Cluster& c) {
    if (c.server >= 0) close(c.server);
    if (c.client >= 0) close(c.client);
    if (c.listener >= 0) close(c.listener);
    c = Cluster{};
}

inline ssize_t receive(int fd) {
    char buf[64] = {};
    return recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
}

struct Exchange {
    int fd = -1;
    ssize_t got = -1;
    bool entered = false;
    char bytes[8] = {};
};

inline void* blocked_reader(void* arg) {
    auto* e = static_cast<Exchange*>(arg);
    e->entered = true;
    e->got = recv(e->fd, e->bytes, sizeof(e->bytes), 0);
    return nullptr;
}
