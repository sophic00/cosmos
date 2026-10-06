#pragma once

// In-process socket layer for a universe (docs/design.md §6). Virtual descriptors never reach the
// kernel, and blocking suspends the calling fiber through the Scheduler, so one universe still
// runs on one OS thread. This is the minimal slice: topology faults (latency shaping, partitions,
// packet shaping beyond the call-level kinds) are P6's.
//
// Flags: MSG_DONTWAIT and MSG_PEEK are honoured on recv, MSG_DONTWAIT on send;
// MSG_WAITALL is ignored (a short read is returned instead of waiting for the full
// length), as are the out-of-band flags.

#include "cosmos/faults.hpp"
#include "cosmos/memory.hpp"
#include "cosmos/task.hpp"
#include "cosmos/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <vector>

namespace cosmos {

// Real fds stay far below this in practice; a colliding workload would need >1000 open files.
constexpr int kVirtualFdBase = 1000;
constexpr size_t kMaxEndpoints = 32;
constexpr size_t kMaxPending = 8;
constexpr size_t kSocketBufferBytes = 64 * 1024;

// What the wrapper decided for one call. `amount` is the rule's magnitude, used by PacketDelay.
struct InjectedFault {
    FaultKind kind = FaultKind::None;
    Duration amount{};
};

struct EndpointState {
    bool known = false;
    bool listener = false;
    bool connected = false;
    bool pending = false;
    bool peer_closed = false;
    bool nonblocking = false;
};

// True for any fd number the virtual layer owns, whether currently open or not.
// Used by wrappers to distinguish virtual fds from host fds without a Simulator.
inline constexpr bool is_virtual_fd(int fd) {
    return fd >= kVirtualFdBase && fd < kVirtualFdBase + static_cast<int>(kMaxEndpoints);
}

class Net {
  public:
    Net(Scheduler& scheduler, VirtualClock& clock);
    Net(const Net&) = delete;
    Net& operator=(const Net&) = delete;

    EndpointState state(int fd) const;

    int socket(int domain, int type, int protocol);
    int pair(int domain, int type, int protocol, int out[2]);
    int bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
    int listen(int fd, int backlog);
    int connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
    int accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
    ssize_t send(int fd, const void* buf, size_t len, int flags, InjectedFault fault);
    ssize_t recv(int fd, void* buf, size_t len, int flags, InjectedFault fault);
    int shutdown(int fd, int how);
    int abort_pending(int fd);
    int close(int fd);
    int poll(struct pollfd* fds, nfds_t count, int timeout_ms);
    int fcntl(int fd, int cmd, long arg);
    int peer_name(int fd, struct sockaddr* addr, socklen_t* addrlen) const;
    int local_name(int fd, struct sockaddr* addr, socklen_t* addrlen) const;

  private:
    struct Packet {
        Time deliver_at{};
        std::vector<uint8_t, RawRealAllocator<uint8_t>> bytes{};
        size_t offset = 0;
    };

    struct Endpoint {
        bool used = false;
        bool listener = false;
        bool connected = false;
        bool awaiting_accept = false;
        bool nonblocking = false;
        bool peer_closed = false;
        uint16_t port = 0;
        // Peer's address as of connect/pair, so getpeername still works after the peer
        // closes and its slot is recycled for an unrelated connection.
        uint16_t peer_port = 0;
        int peer = -1;
        size_t rx_bytes = 0;
        std::vector<Packet, RawRealAllocator<Packet>> rx{};
        std::vector<int, RawRealAllocator<int>> pending{};
    };

    Endpoint* get(int fd);
    const Endpoint* get(int fd) const;
    // Peer endpoint only when the slot still holds the same connection: used and linked
    // back. A stale index (peer closed and slot recycled) returns nullptr so callers
    // never touch an unrelated connection's bytes.
    Endpoint* peer_of(Endpoint& ep);
    const Endpoint* peer_of(const Endpoint& ep) const;
    bool is_known(int fd) const { return get(fd) != nullptr; }
    uint16_t alloc_port();
    Endpoint* find_port_owner(uint16_t port);
    int create_endpoint();
    Endpoint* find_listener(uint16_t port);
    void release(Endpoint& ep);
    bool readable(const Endpoint& ep) const;
    bool writable(const Endpoint& ep) const;
    bool room_for(const Endpoint& ep, size_t count) const;
    void signal_change();
    void enqueue(Endpoint& peer, InjectedFault fault, const void* buf, size_t len, Time now);

    Scheduler& scheduler_;
    VirtualClock& clock_;
    std::array<Endpoint, kMaxEndpoints> endpoints_{};
    uint16_t next_port_ = 40000;
    pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond_ = PTHREAD_COND_INITIALIZER;
};

} // namespace cosmos
