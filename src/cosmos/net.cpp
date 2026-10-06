#include "cosmos/net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace cosmos {

namespace {

void write_addr(struct sockaddr* addr, socklen_t* addrlen, uint16_t port) {
    if (addr == nullptr || addrlen == nullptr) return;
    struct sockaddr_in full{};
    full.sin_family = AF_INET;
    full.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    full.sin_port = htons(port);
    const socklen_t available = *addrlen;
    *addrlen = sizeof(full);
    if (available >= static_cast<socklen_t>(sizeof(full))) {
        std::memcpy(addr, &full, sizeof(full));
        return;
    }
    std::memcpy(addr, &full, static_cast<size_t>(available));
}

} // namespace

Net::Net(Scheduler& scheduler, VirtualClock& clock) : scheduler_(scheduler), clock_(clock) {}

Net::Endpoint* Net::get(int fd) {
    const int index = fd - kVirtualFdBase;
    if (index < 0 || static_cast<size_t>(index) >= kMaxEndpoints) return nullptr;
    Endpoint& ep = endpoints_[static_cast<size_t>(index)];
    return ep.used ? &ep : nullptr;
}

const Net::Endpoint* Net::get(int fd) const { return const_cast<Net*>(this)->get(fd); }

Net::Endpoint* Net::peer_of(Endpoint& ep) {
    if (ep.peer < 0 || static_cast<size_t>(ep.peer) >= kMaxEndpoints) return nullptr;
    Endpoint& peer = endpoints_[static_cast<size_t>(ep.peer)];
    if (!peer.used) return nullptr;
    const int self = static_cast<int>(&ep - endpoints_.data());
    if (peer.peer != self) return nullptr;
    return &peer;
}

const Net::Endpoint* Net::peer_of(const Endpoint& ep) const {
    if (ep.peer < 0 || static_cast<size_t>(ep.peer) >= kMaxEndpoints) return nullptr;
    const Endpoint& peer = endpoints_[static_cast<size_t>(ep.peer)];
    if (!peer.used) return nullptr;
    const int self = static_cast<int>(&ep - endpoints_.data());
    if (peer.peer != self) return nullptr;
    return &peer;
}

uint16_t Net::alloc_port() {
    // Skip 0 (reads as unbound) and any port currently owned, so ephemeral allocation
    // never collides with an explicit bind and wrap-around cannot alias.
    for (size_t tries = 0; tries < 65536; ++tries) {
        uint16_t candidate = next_port_++;
        if (next_port_ == 0) next_port_ = 1;
        if (candidate == 0) continue;
        if (find_port_owner(candidate) == nullptr) return candidate;
    }
    // Extremely full (64k tries failed): fall back to incrementing; bind will report
    // EADDRINUSE on collision.
    uint16_t fallback = next_port_++;
    if (fallback == 0) {
        fallback = 1;
        next_port_ = 2;
    }
    return fallback;
}

Net::Endpoint* Net::find_port_owner(uint16_t port) {
    if (port == 0) return nullptr;
    for (Endpoint& ep : endpoints_) {
        if (ep.used && ep.port == port) return &ep;
    }
    return nullptr;
}

EndpointState Net::state(int fd) const {
    EndpointState out;
    const Endpoint* ep = get(fd);
    if (ep == nullptr) return out;
    out.known = true;
    out.listener = ep->listener;
    out.connected = ep->connected;
    out.pending = !ep->pending.empty();
    out.peer_closed = ep->peer_closed;
    out.nonblocking = ep->nonblocking;
    return out;
}

int Net::create_endpoint() {
    for (size_t i = 0; i < kMaxEndpoints; ++i) {
        Endpoint& ep = endpoints_[i];
        if (ep.used) continue;
        ep = Endpoint{};
        ep.used = true;
        return kVirtualFdBase + static_cast<int>(i);
    }
    errno = EMFILE;
    return -1;
}

int Net::socket(int domain, int type, int protocol) {
    if (domain != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if ((type & SOCK_STREAM) != SOCK_STREAM) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (protocol != 0 && protocol != IPPROTO_TCP) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    const int fd = create_endpoint();
    if (fd < 0) return -1;
    get(fd)->nonblocking = (type & SOCK_NONBLOCK) != 0;
    return fd;
}

int Net::pair(int domain, int type, int protocol, int out[2]) {
    if (domain != AF_UNIX && domain != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if ((type & SOCK_STREAM) != SOCK_STREAM || protocol != 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    const int left = create_endpoint();
    if (left < 0) return -1;
    const int right = create_endpoint();
    if (right < 0) {
        release(*get(left));
        return -1;
    }
    Endpoint& a = *get(left);
    Endpoint& b = *get(right);
    a.connected = true;
    b.connected = true;
    a.nonblocking = (type & SOCK_NONBLOCK) != 0;
    b.nonblocking = a.nonblocking;
    a.peer = right - kVirtualFdBase;
    b.peer = left - kVirtualFdBase;
    a.port = alloc_port();
    b.port = alloc_port();
    a.peer_port = b.port;
    b.peer_port = a.port;
    signal_change();
    out[0] = left;
    out[1] = right;
    return 0;
}

int Net::bind(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (ep->connected) {
        errno = EINVAL;
        return -1;
    }
    if (addr == nullptr || addrlen < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    if (addr->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (ep->port != 0) {
        errno = EINVAL;
        return -1;
    }
    const uint16_t requested = ntohs(reinterpret_cast<const struct sockaddr_in*>(addr)->sin_port);
    if (requested != 0 && find_port_owner(requested) != nullptr) {
        errno = EADDRINUSE;
        return -1;
    }
    ep->port = requested == 0 ? alloc_port() : requested;
    return 0;
}

int Net::listen(int fd, int backlog) {
    (void)backlog;
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (ep->connected) {
        errno = EINVAL;
        return -1;
    }
    ep->listener = true;
    return 0;
}

Net::Endpoint* Net::find_listener(uint16_t port) {
    for (Endpoint& ep : endpoints_) {
        if (ep.used && ep.listener && ep.port == port) return &ep;
    }
    return nullptr;
}

int Net::connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (ep->connected) {
        errno = EISCONN;
        return -1;
    }
    if (addr == nullptr || addrlen < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    if (addr->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    const uint16_t port = ntohs(reinterpret_cast<const struct sockaddr_in*>(addr)->sin_port);
    Endpoint* listener = find_listener(port);
    if (listener == nullptr || listener->pending.size() >= kMaxPending) {
        errno = ECONNREFUSED;
        return -1;
    }
    const int server_fd = create_endpoint();
    if (server_fd < 0) return -1;

    Endpoint& client = *ep;
    Endpoint& server = *get(server_fd);
    client.connected = true;
    client.port = alloc_port();
    client.peer = server_fd - kVirtualFdBase;
    server.connected = true;
    server.awaiting_accept = true;
    server.port = listener->port;
    server.peer = fd - kVirtualFdBase;
    client.peer_port = server.port;
    server.peer_port = client.port;
    listener->pending.push_back(server_fd - kVirtualFdBase);
    signal_change();
    return 0;
}

int Net::accept(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->listener) {
        errno = EINVAL;
        return -1;
    }
    while (ep->pending.empty()) {
        if (ep->nonblocking) {
            errno = EAGAIN;
            return -1;
        }
        scheduler_.mutex_lock(&mu_);
        while (ep->pending.empty())
            scheduler_.cond_wait(&cond_, &mu_);
        scheduler_.mutex_unlock(&mu_);
    }
    const int server_index = ep->pending.front();
    ep->pending.erase(ep->pending.begin());
    Endpoint& server = endpoints_[static_cast<size_t>(server_index)];
    server.awaiting_accept = false;
    const Endpoint* peer = peer_of(server);
    write_addr(addr, addrlen, peer != nullptr ? peer->port : server.peer_port);
    return kVirtualFdBase + server_index;
}

bool Net::readable(const Endpoint& ep) const {
    if (ep.listener) return !ep.pending.empty();
    if (!ep.rx.empty() && ep.rx.front().deliver_at <= clock_.now()) return true;
    return ep.peer_closed;
}

bool Net::room_for(const Endpoint& ep, size_t count) const {
    return ep.rx_bytes + count <= kSocketBufferBytes;
}

bool Net::writable(const Endpoint& ep) const {
    if (!ep.connected || ep.peer < 0) return false;
    const Endpoint* peer = peer_of(ep);
    if (peer == nullptr) return false;
    return room_for(*peer, 1);
}

void Net::signal_change() {
    // Deliberately unlocked: the scheduler is cooperative, so no waiter can be preempted between
    // its predicate check and its cond registration, and the broadcast cannot miss one.
    scheduler_.cond_broadcast(&cond_);
}

void Net::enqueue(Endpoint& peer, InjectedFault fault, const void* buf, size_t len, Time now) {
    Packet packet;
    packet.bytes.assign(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + len);
    if (fault.kind == FaultKind::PacketCorrupt) {
        for (uint8_t& byte : packet.bytes)
            byte ^= 0x01;
    }
    packet.deliver_at = fault.kind == FaultKind::PacketDelay ? now + fault.amount : now;
    peer.rx_bytes += len;
    if (fault.kind == FaultKind::PacketReorder && !peer.rx.empty()) {
        peer.rx.insert(peer.rx.end() - 1, std::move(packet));
        return;
    }
    peer.rx.push_back(std::move(packet));
}

ssize_t Net::send(int fd, const void* buf, size_t len, int flags, InjectedFault fault) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->connected || ep->peer < 0) {
        errno = ENOTCONN;
        return -1;
    }
    // Real sockets also raise SIGPIPE here; a fiber cannot, and MSG_NOSIGNAL callers cannot tell.
    if (ep->peer_closed) {
        errno = EPIPE;
        return -1;
    }
    Endpoint* peer = peer_of(*ep);
    if (peer == nullptr) {
        // Peer slot went away without a clean close path (stale index): the peer is gone,
        // so a send is a broken pipe rather than a use of whatever recycled the slot.
        errno = EPIPE;
        return -1;
    }
    const size_t count = fault.kind == FaultKind::ShortSend ? (len >= 2 ? len / 2 : len) : len;
    if (count == 0) return 0;
    if (fault.kind == FaultKind::PacketDrop) return static_cast<ssize_t>(count);

    const bool nonblocking = ep->nonblocking || (flags & MSG_DONTWAIT) != 0;
    while (!room_for(*peer, count)) {
        if (nonblocking) {
            errno = EAGAIN;
            return -1;
        }
        scheduler_.mutex_lock(&mu_);
        // Re-resolve after waking: the peer may have drained, closed, or been recycled,
        // and this endpoint itself may have been closed.
        Endpoint* fresh_peer = peer_of(*ep);
        bool has_room = fresh_peer != nullptr && room_for(*fresh_peer, count);
        while (ep->used && !has_room && !ep->peer_closed) {
            scheduler_.cond_wait(&cond_, &mu_);
            fresh_peer = peer_of(*ep);
            has_room = fresh_peer != nullptr && room_for(*fresh_peer, count);
        }
        scheduler_.mutex_unlock(&mu_);
        ep = get(fd);
        if (ep == nullptr) {
            errno = EBADF;
            return -1;
        }
        if (ep->peer_closed || peer_of(*ep) == nullptr) {
            errno = EPIPE;
            return -1;
        }
        peer = peer_of(*ep);
    }

    enqueue(*peer, fault, buf, count, clock_.now());
    signal_change();
    return static_cast<ssize_t>(count);
}

ssize_t Net::recv(int fd, void* buf, size_t len, int flags, InjectedFault fault) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->connected) {
        errno = ENOTCONN;
        return -1;
    }
    if (fault.kind == FaultKind::ConnReset) {
        errno = ECONNRESET;
        return -1;
    }
    // Marks the EOF and falls through: a real socket delivers what is already buffered first.
    if (fault.kind == FaultKind::PeerClose) ep->peer_closed = true;
    // A zero-length recv is not a read: the real call returns 0 without waiting for data.
    if (len == 0) return 0;
    const bool nonblocking = ep->nonblocking || (flags & MSG_DONTWAIT) != 0;
    while (true) {
        if (!ep->rx.empty()) {
            Packet& packet = ep->rx.front();
            if (packet.deliver_at > clock_.now()) {
                if (nonblocking) {
                    errno = EAGAIN;
                    return -1;
                }
                const int64_t wait_ns = (packet.deliver_at - clock_.now()).ns;
                struct timespec req{wait_ns / 1'000'000'000LL, wait_ns % 1'000'000'000LL};
                scheduler_.sleep_ns(&req, nullptr);
                continue;
            }
            const size_t available = packet.bytes.size() - packet.offset;
            const size_t count = available < len ? available : len;
            std::memcpy(buf, packet.bytes.data() + packet.offset, count);
            if ((flags & MSG_PEEK) == 0) {
                packet.offset += count;
                ep->rx_bytes -= count;
                if (packet.offset == packet.bytes.size()) ep->rx.erase(ep->rx.begin());
                // Any consumption frees room, and a send blocked on room must be woken by it.
                signal_change();
            }
            return static_cast<ssize_t>(count);
        }
        if (ep->peer_closed) return 0;
        if (nonblocking) {
            errno = EAGAIN;
            return -1;
        }
        scheduler_.mutex_lock(&mu_);
        while (ep->rx.empty() && !ep->peer_closed)
            scheduler_.cond_wait(&cond_, &mu_);
        scheduler_.mutex_unlock(&mu_);
    }
}

int Net::abort_pending(int fd) {
    Endpoint* ep = get(fd);
    if (ep == nullptr || !ep->listener || ep->pending.empty()) {
        errno = EINVAL;
        return -1;
    }
    const int index = ep->pending.front();
    ep->pending.erase(ep->pending.begin());
    release(endpoints_[static_cast<size_t>(index)]);
    signal_change();
    return 0;
}

int Net::shutdown(int fd, int how) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->connected || ep->peer < 0) {
        errno = ENOTCONN;
        return -1;
    }
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
        errno = EINVAL;
        return -1;
    }
    if (how == SHUT_RD || how == SHUT_RDWR) {
        ep->rx.clear();
        ep->rx_bytes = 0;
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        // Only touch the live peer: a stale index means the peer already went away
        // (its peer_closed was set at close time), so there is nothing left to mark.
        if (Endpoint* peer = peer_of(*ep)) {
            peer->peer_closed = true;
        }
    }
    signal_change();
    return 0;
}

void Net::release(Endpoint& ep) {
    // Mark the live peer closed, but never touch a recycled slot: peer_of validates
    // the back-link, so a stale index is left alone.
    if (Endpoint* peer = peer_of(ep)) {
        peer->peer_closed = true;
    }
    for (int pending : ep.pending) {
        if (pending < 0 || static_cast<size_t>(pending) >= kMaxEndpoints) continue;
        Endpoint& server = endpoints_[static_cast<size_t>(pending)];
        if (server.used) release(server);
    }
    ep = Endpoint{};
}

int Net::close(int fd) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    release(*ep);
    signal_change();
    return 0;
}

int Net::poll(struct pollfd* fds, nfds_t count, int timeout_ms) {
    const Time start = clock_.now();
    while (true) {
        int ready = 0;
        for (nfds_t i = 0; i < count; ++i) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            const Endpoint* ep = get(fds[i].fd);
            if (ep == nullptr) {
                // Closed virtual fd: report POLLNVAL like the kernel. Anything else
                // under a universe is a host descriptor (regular file), always ready.
                if (is_virtual_fd(fds[i].fd)) {
                    fds[i].revents = POLLNVAL;
                    ++ready;
                    continue;
                }
                fds[i].revents = static_cast<short>(fds[i].events & (POLLIN | POLLOUT));
                if (fds[i].revents != 0) ++ready;
                continue;
            }
            short revents = 0;
            if ((fds[i].events & POLLIN) != 0 && readable(*ep)) revents |= POLLIN;
            if ((fds[i].events & POLLOUT) != 0 && writable(*ep)) revents |= POLLOUT;
            if (ep->peer_closed) revents |= POLLHUP;
            fds[i].revents = revents;
            if (revents != 0) ++ready;
        }
        if (ready > 0 || count == 0 || timeout_ms == 0) return ready;

        const Time now = clock_.now();
        // Earliest future deliver_at among POLLIN-watched virtual fds. A delayed packet
        // is not readable yet, but it will become readable at deliver_at without any
        // broadcast, so parking on the cond alone would deadlock.
        bool has_delayed = false;
        Time earliest = Time::max();
        for (nfds_t i = 0; i < count; ++i) {
            if (fds[i].fd < 0 || (fds[i].events & POLLIN) == 0) continue;
            const Endpoint* ep = get(fds[i].fd);
            if (ep == nullptr || ep->listener || ep->rx.empty()) continue;
            const Time deliver = ep->rx.front().deliver_at;
            if (deliver > now && (!has_delayed || deliver < earliest)) {
                has_delayed = true;
                earliest = deliver;
            }
        }

        if (timeout_ms < 0) {
            if (has_delayed) {
                // Sleep until the packet lands (like recv does), then re-check.
                const int64_t wait_ns = (earliest - now).ns;
                struct timespec req{wait_ns / 1'000'000'000LL, wait_ns % 1'000'000'000LL};
                scheduler_.sleep_ns(&req, nullptr);
                continue;
            }
            scheduler_.mutex_lock(&mu_);
            // Full readiness: POLLIN-readable, POLLOUT-writable, or HUP/NVAL. Checking
            // readable alone would never wake a POLLOUT-only waiter on a full buffer.
            bool any = false;
            for (nfds_t i = 0; i < count && !any; ++i) {
                if (fds[i].fd < 0) continue;
                const Endpoint* ep = get(fds[i].fd);
                if (ep == nullptr) {
                    any = true;
                    continue;
                }
                short want = fds[i].events;
                if ((want & POLLIN) != 0 && readable(*ep)) any = true;
                if ((want & POLLOUT) != 0 && writable(*ep)) any = true;
                if (ep->peer_closed) any = true;
            }
            if (!any) scheduler_.cond_wait(&cond_, &mu_);
            scheduler_.mutex_unlock(&mu_);
            continue;
        }

        const int64_t elapsed_ms = (now - start).ns / 1'000'000LL;
        if (elapsed_ms >= timeout_ms) return 0;
        const Time deadline = start + Duration{timeout_ms * 1'000'000LL};
        // Wake at the earlier of the poll deadline and the next delayed delivery, so a
        // PacketDelay earlier than the timeout does not advance time spuriously past it.
        // Note: data sent by another fiber during this sleep wakes an infinite poll via
        // broadcast but not a finite one (no timed cond wait); the finite poll still
        // returns the data, just at the sleep boundary rather than the send instant.
        const Time wake_at = (has_delayed && earliest < deadline) ? earliest : deadline;
        const int64_t wait_ns = (wake_at - now).ns;
        if (wait_ns <= 0) continue;
        struct timespec req{wait_ns / 1'000'000'000LL, wait_ns % 1'000'000'000LL};
        scheduler_.sleep_ns(&req, nullptr);
    }
}

int Net::fcntl(int fd, int cmd, long arg) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    switch (cmd) {
    case F_GETFL:
        return O_RDWR | (ep->nonblocking ? O_NONBLOCK : 0);
    case F_SETFL:
        ep->nonblocking = (arg & O_NONBLOCK) != 0;
        return 0;
    case F_GETFD:
        return 0;
    case F_SETFD:
        return 0;
    default:
        // Dup/lease/lock commands have no meaning for a virtual endpoint; P6 owns them.
        errno = EINVAL;
        return -1;
    }
}

int Net::peer_name(int fd, struct sockaddr* addr, socklen_t* addrlen) const {
    const Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->connected || ep->peer < 0) {
        errno = ENOTCONN;
        return -1;
    }
    // Cached at connect/pair time: the live slot may have been recycled since.
    write_addr(addr, addrlen, ep->peer_port);
    return 0;
}

int Net::local_name(int fd, struct sockaddr* addr, socklen_t* addrlen) const {
    const Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    write_addr(addr, addrlen, ep->port);
    return 0;
}

} // namespace cosmos
