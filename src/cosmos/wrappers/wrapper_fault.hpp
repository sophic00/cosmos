#pragma once

// Header-only on purpose: each __wrap_* must stay in its own section so --gc-sections can discard
// an unused wrapper without dragging in the others (docs/design.md §2).

#include "cosmos/faults.hpp"
// For kVirtualFdBase: the storage surface must not treat the socket layer's fds as files.
#include "cosmos/net.hpp"

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>

namespace cosmos::wrappers {

// Set while a wrapper runs its own logic: a wrapped call arriving now is engine work and must pass
// through unfaulted (Rule 7).
inline thread_local bool in_wrapper_logic = false;

struct ReentrancyGuard {
    // Restores rather than clears, so an inner guard cannot unguard an outer one.
    ReentrancyGuard() : previous_(in_wrapper_logic) { in_wrapper_logic = true; }
    ~ReentrancyGuard() { in_wrapper_logic = previous_; }

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;

  private:
    bool previous_;
};

// Call once per eligible wrapped call and never on a passthrough path (Rule 3).
template <typename Sim> FaultKind decide_for(Sim* sim, FaultClass cls, SiteId site) {
    if (sim == nullptr) {
        return FaultKind::None;
    }
    if constexpr (requires { sim->injector_or_null()->decide(cls, site); }) {
        if (auto* injector = sim->injector_or_null()) {
            return injector->decide(cls, site);
        }
    }
    return FaultKind::None;
}

// What a wrapper must translate: the fault that fired, plus the strength only some faults have
// (a packet delay, a clock step, an interrupted sleep's elapsed part).
struct Decision {
    FaultKind kind = FaultKind::None;
    Duration amount{};
};

// Callers hold the wrapper's ReentrancyGuard: this reads engine state (Rule 7).
template <typename Sim> Decision decide_with_amount(Sim* sim, FaultClass cls, SiteId site) {
    Decision fault;
    fault.kind = decide_for(sim, cls, site);
    if (fault.kind == FaultKind::None) {
        return fault;
    }
    if (const FaultRule* rule = sim->injector_or_null()->config().rule_for(site)) {
        fault.amount = rule->amount;
    }
    return fault;
}

// A call is eligible only where a decision could produce a legal, observable outcome (Rule 15).
// Standard streams are excluded so logging cannot consume Storage draws or fail with injected
// errors; an empty transfer has nothing to observe. 1-byte writes stay eligible.
// A cosmos virtual fd (the simulated socket layer) is not a file descriptor the storage surface
// owns: reading or writing one through the storage wrappers would spend Storage draws on a fault
// the file API cannot honour, and then reach the host with a number the kernel never issued.
// Only the virtual window is excluded: host workloads with >1000 open files must still draw.
inline constexpr bool storage_fd_eligible(int fd) { return fd > 2 && !cosmos::is_virtual_fd(fd); }

inline constexpr bool storage_read_eligible(int fd, size_t count) {
    return storage_fd_eligible(fd) && count > 0;
}

inline constexpr bool storage_write_eligible(int fd, size_t count) {
    return storage_fd_eligible(fd) && count > 0;
}

// A non-IPv4 address is answered by the transport before the injector, like a calloc overflow:
// the observable is a real API failure rather than a fault (Rule 15).
inline constexpr bool network_addr_eligible(const struct sockaddr* addr, socklen_t addrlen) {
    return addr != nullptr && addrlen >= sizeof(struct sockaddr_in) && addr->sa_family == AF_INET;
}

// An accept with nothing queued has no abort observable; it blocks or reports EAGAIN instead.
inline constexpr bool network_accept_eligible(bool pending) { return pending; }

inline constexpr bool network_send_eligible(bool connected, size_t len) {
    return connected && len > 0;
}

inline constexpr bool network_recv_eligible(bool connected, size_t len) {
    return connected && len > 0;
}

// Nanoseconds in a timespec, or -1 if it is not a legal POSIX request (negative seconds, or a
// nanosecond field outside [0, 1e9)). Taken by value so it stays usable in constant expressions
// under sanitizers, which do not keep globals constant-initialized. Saturates like every other
// time conversion here.
inline constexpr int64_t clock_timespec_ns(struct timespec ts) {
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1'000'000'000L) return -1;
    return add_sat(mul_sat(static_cast<int64_t>(ts.tv_sec), 1'000'000'000LL),
                   static_cast<int64_t>(ts.tv_nsec));
}

// A sleep that is missing, malformed, or zero-length has nothing to interrupt, so it is answered
// by the scheduler's own validation and must not spend a Clock draw (Rules 3 and 15).
inline constexpr bool clock_sleep_eligible(const struct timespec* req) {
    return req != nullptr && clock_timespec_ns(*req) > 0;
}

// Eligible because C11 lets malloc(0) return nullptr, so a fire there is still a legal observable.
inline constexpr bool memory_alloc_eligible(size_t) { return true; }

// A product overflow is a real API failure, not a fault, so it must never reach the injector.
inline constexpr bool memory_calloc_eligible(size_t nmemb, size_t size) {
    return nmemb == 0 || size <= SIZE_MAX / nmemb;
}

// realloc(ptr, 0) is a free, and a block this universe does not own is not its call to fault.
inline constexpr bool memory_realloc_eligible(const void* ptr, size_t size, bool owned_by_sim) {
    if (size == 0) return false;
    return ptr == nullptr || owned_by_sim;
}

} // namespace cosmos::wrappers
