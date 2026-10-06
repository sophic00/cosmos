#include "cosmos/cosmos.hpp"

#include "wrapper_fault.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <unistd.h>

// The virtual socket layer (src/cosmos/net.cpp) owns the transport; these wrappers own only the
// passthrough check and the kind-to-errno translation §8.2 assigns to them. Kinds whose observable
// is transport state rather than a single errno (ShortSend, Packet*, PeerClose) go through to the
// layer, which is the only place that can honour them.

namespace {

cosmos::Net* current_net() { return &cosmos::Simulator::current()->net(); }

cosmos::InjectedFault decide(cosmos::Simulator* sim, cosmos::SiteId site) {
    cosmos::wrappers::ReentrancyGuard guard;
    const cosmos::wrappers::Decision decision =
        cosmos::wrappers::decide_with_amount(sim, cosmos::FaultClass::Network, site);
    return cosmos::InjectedFault{decision.kind, decision.amount};
}

} // namespace

extern "C" {

int __real_socket(int domain, int type, int protocol);
int __real_socketpair(int domain, int type, int protocol, int sv[2]);
int __real_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int __real_listen(int sockfd, int backlog);
int __real_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int __real_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
ssize_t __real_send(int sockfd, const void* buf, size_t len, int flags);
ssize_t __real_recv(int sockfd, void* buf, size_t len, int flags);
ssize_t __real___recv_chk(int fd, void* buf, size_t len, size_t buflen, int flags);
int __real_shutdown(int sockfd, int how);
int __real_close(int fd);
int __real_poll(struct pollfd* fds, nfds_t nfds, int timeout);
int __real___poll_chk(struct pollfd* fds, nfds_t nfds, int timeout, size_t fdslen);
int __real_fcntl(int fd, int cmd, ...);
int __real_getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int __real_getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int __real_setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);
int __real_getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen);

int __wrap_socket(int domain, int type, int protocol) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_socket(domain, type, protocol);
    }
    // Only IPv4 STREAM/TCP is virtualized; anything else (AF_UNIX, NETLINK, DGRAM, ...)
    // belongs to the host. Failing it with EAFNOSUPPORT here would break host use
    // under a universe.
    if (domain != AF_INET || (type & SOCK_STREAM) != SOCK_STREAM ||
        (protocol != 0 && protocol != IPPROTO_TCP)) {
        return __real_socket(domain, type, protocol);
    }
    return current_net()->socket(domain, type, protocol);
}

int __wrap_socketpair(int domain, int type, int protocol, int sv[2]) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_socketpair(domain, type, protocol, sv);
    }
    if ((domain != AF_UNIX && domain != AF_INET) || (type & SOCK_STREAM) != SOCK_STREAM ||
        protocol != 0) {
        return __real_socketpair(domain, type, protocol, sv);
    }
    return current_net()->pair(domain, type, protocol, sv);
}

int __wrap_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    if (!cosmos::Simulator::has_current()) return __real_bind(sockfd, addr, addrlen);
    if (!current_net()->state(sockfd).known) return __real_bind(sockfd, addr, addrlen);
    return current_net()->bind(sockfd, addr, addrlen);
}

int __wrap_listen(int sockfd, int backlog) {
    if (!cosmos::Simulator::has_current()) return __real_listen(sockfd, backlog);
    if (!current_net()->state(sockfd).known) return __real_listen(sockfd, backlog);
    return current_net()->listen(sockfd, backlog);
}

int __wrap_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    if (!cosmos::Simulator::has_current()) return __real_connect(sockfd, addr, addrlen);
    cosmos::Simulator* sim = cosmos::Simulator::current();
    const cosmos::EndpointState st = current_net()->state(sockfd);
    if (!st.known) return __real_connect(sockfd, addr, addrlen);
    if (cosmos::wrappers::in_wrapper_logic || st.connected ||
        !cosmos::wrappers::network_addr_eligible(addr, addrlen)) {
        return current_net()->connect(sockfd, addr, addrlen);
    }
    const cosmos::InjectedFault fault = decide(sim, cosmos::SiteId::connect);
    switch (fault.kind) {
    case cosmos::FaultKind::ConnRefused:
        errno = ECONNREFUSED;
        return -1;
    case cosmos::FaultKind::ConnReset:
        // A reset mid-handshake has no reset observable on connect(); ETIMEDOUT is its legal peer.
        errno = ETIMEDOUT;
        return -1;
    default:
        break;
    }
    return current_net()->connect(sockfd, addr, addrlen);
}

int __wrap_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    if (!cosmos::Simulator::has_current()) return __real_accept(sockfd, addr, addrlen);
    cosmos::Simulator* sim = cosmos::Simulator::current();
    const cosmos::EndpointState st = current_net()->state(sockfd);
    if (!st.known) return __real_accept(sockfd, addr, addrlen);
    if (cosmos::wrappers::in_wrapper_logic || !st.listener ||
        !cosmos::wrappers::network_accept_eligible(st.pending)) {
        return current_net()->accept(sockfd, addr, addrlen);
    }
    const cosmos::InjectedFault fault = decide(sim, cosmos::SiteId::accept);
    if (fault.kind == cosmos::FaultKind::ConnReset) {
        // accept(2) has no reset observable; an aborted queued connection drops off the queue.
        current_net()->abort_pending(sockfd);
        errno = ECONNABORTED;
        return -1;
    }
    return current_net()->accept(sockfd, addr, addrlen);
}

ssize_t __wrap_send(int sockfd, const void* buf, size_t len, int flags) {
    if (!cosmos::Simulator::has_current()) return __real_send(sockfd, buf, len, flags);
    cosmos::Simulator* sim = cosmos::Simulator::current();
    const cosmos::EndpointState st = current_net()->state(sockfd);
    if (!st.known) return __real_send(sockfd, buf, len, flags);
    if (cosmos::wrappers::in_wrapper_logic ||
        !cosmos::wrappers::network_send_eligible(st.connected, len)) {
        return current_net()->send(sockfd, buf, len, flags, cosmos::InjectedFault{});
    }
    const cosmos::InjectedFault fault = decide(sim, cosmos::SiteId::send);
    if (fault.kind == cosmos::FaultKind::ConnReset) {
        errno = ECONNRESET;
        return -1;
    }
    return current_net()->send(sockfd, buf, len, flags, fault);
}

ssize_t __wrap_recv(int sockfd, void* buf, size_t len, int flags) {
    if (!cosmos::Simulator::has_current()) return __real_recv(sockfd, buf, len, flags);
    cosmos::Simulator* sim = cosmos::Simulator::current();
    const cosmos::EndpointState st = current_net()->state(sockfd);
    if (!st.known) return __real_recv(sockfd, buf, len, flags);
    if (cosmos::wrappers::in_wrapper_logic ||
        !cosmos::wrappers::network_recv_eligible(st.connected, len)) {
        return current_net()->recv(sockfd, buf, len, flags, cosmos::InjectedFault{});
    }
    const cosmos::InjectedFault fault = decide(sim, cosmos::SiteId::recv);
    if (fault.kind == cosmos::FaultKind::ConnReset) {
        errno = ECONNRESET;
        return -1;
    }
    return current_net()->recv(sockfd, buf, len, flags, fault);
}

// glibc substitutes __recv_chk for recv() wherever the destination size is known and fortification
// is on, which -fsanitize=address enables by itself. --wrap=recv does not see that symbol, so
// without this the site is skipped in exactly the builds that are supposed to catch defects.
ssize_t __wrap___recv_chk(int fd, void* buf, size_t len, size_t buflen, int flags) {
    if (!cosmos::Simulator::has_current()) return __real___recv_chk(fd, buf, len, buflen, flags);
    cosmos::Simulator* sim = cosmos::Simulator::current();
    const cosmos::EndpointState st = current_net()->state(fd);
    if (!st.known) return __real___recv_chk(fd, buf, len, buflen, flags);
    if (cosmos::wrappers::in_wrapper_logic ||
        !cosmos::wrappers::network_recv_eligible(st.connected, len)) {
        return current_net()->recv(fd, buf, len, flags, cosmos::InjectedFault{});
    }
    const cosmos::InjectedFault fault = decide(sim, cosmos::SiteId::recv);
    if (fault.kind == cosmos::FaultKind::ConnReset) {
        errno = ECONNRESET;
        return -1;
    }
    return current_net()->recv(fd, buf, len, flags, fault);
}

int __wrap_shutdown(int sockfd, int how) {
    if (!cosmos::Simulator::has_current() || !current_net()->state(sockfd).known) {
        return __real_shutdown(sockfd, how);
    }
    return current_net()->shutdown(sockfd, how);
}

int __wrap_close(int fd) {
    if (cosmos::Simulator::has_current() && current_net()->state(fd).known) {
        return current_net()->close(fd);
    }
    return __real_close(fd);
}

int __wrap_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    if (!cosmos::Simulator::has_current()) return __real_poll(fds, nfds, timeout);
    // Host-only poll keeps host semantics (pipes/FIFOs would busy-loop as "always
    // ready" under the virtual poll). Mixed or virtual polls go to the transport.
    bool any_virtual = false;
    for (nfds_t i = 0; fds != nullptr && i < nfds; ++i) {
        if (fds[i].fd >= 0 &&
            (current_net()->state(fds[i].fd).known || cosmos::is_virtual_fd(fds[i].fd))) {
            any_virtual = true;
            break;
        }
    }
    if (!any_virtual) return __real_poll(fds, nfds, timeout);
    return current_net()->poll(fds, nfds, timeout);
}

int __wrap___poll_chk(struct pollfd* fds, nfds_t nfds, int timeout, size_t fdslen) {
    if (!cosmos::Simulator::has_current()) return __real___poll_chk(fds, nfds, timeout, fdslen);
    bool any_virtual = false;
    for (nfds_t i = 0; fds != nullptr && i < nfds; ++i) {
        if (fds[i].fd >= 0 &&
            (current_net()->state(fds[i].fd).known || cosmos::is_virtual_fd(fds[i].fd))) {
            any_virtual = true;
            break;
        }
    }
    if (!any_virtual) return __real___poll_chk(fds, nfds, timeout, fdslen);
    return current_net()->poll(fds, nfds, timeout);
}

int __wrap_fcntl(int fd, int cmd, ...) {
    bool takes_arg = false;
    switch (cmd) {
    case F_SETFL:
    case F_SETFD:
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        takes_arg = true;
        break;
    default:
        break;
    }
    int arg = 0;
    if (takes_arg) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, int);
        va_end(ap);
    }
    if (!cosmos::Simulator::has_current() || !current_net()->state(fd).known) {
        return __real_fcntl(fd, cmd, arg);
    }
    return current_net()->fcntl(fd, cmd, arg);
}

int __wrap_getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    if (!cosmos::Simulator::has_current() || !current_net()->state(sockfd).known) {
        return __real_getsockname(sockfd, addr, addrlen);
    }
    return current_net()->local_name(sockfd, addr, addrlen);
}

int __wrap_getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    if (!cosmos::Simulator::has_current() || !current_net()->state(sockfd).known) {
        return __real_getpeername(sockfd, addr, addrlen);
    }
    return current_net()->peer_name(sockfd, addr, addrlen);
}

// Socket options describe a real stack we are not using; a virtual endpoint accepts and ignores
// them so the ordinary socket/bind/listen/setsockopt dance keeps working.
int __wrap_setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
    if (!cosmos::Simulator::has_current() || !current_net()->state(sockfd).known) {
        return __real_setsockopt(sockfd, level, optname, optval, optlen);
    }
    return 0;
}

int __wrap_getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
    if (!cosmos::Simulator::has_current() || !current_net()->state(sockfd).known) {
        return __real_getsockopt(sockfd, level, optname, optval, optlen);
    }
    if (optname == SO_ERROR && optval != nullptr && optlen != nullptr &&
        *optlen >= static_cast<socklen_t>(sizeof(int))) {
        *reinterpret_cast<int*>(optval) = 0;
        *optlen = sizeof(int);
    }
    return 0;
}

} // extern "C"
