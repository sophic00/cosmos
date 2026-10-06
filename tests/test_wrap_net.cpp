#include "net_test_support.hpp"

#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>

extern "C" int __wrap___poll_chk(struct pollfd* fds, nfds_t nfds, int timeout, size_t fdslen);

namespace {

uint16_t local_port_of(int fd) {
    Address addr;
    socklen_t len = addr.size();
    must(getsockname(fd, addr.as_mutable_sockaddr(), &len) == 0);
    return ntohs(addr.raw.sin_port);
}

uint16_t peer_port_of(int fd) {
    Address addr;
    socklen_t len = addr.size();
    must(getpeername(fd, addr.as_mutable_sockaddr(), &len) == 0);
    return ntohs(addr.raw.sin_port);
}

} // namespace

void test_passthrough_without_universe() {
    must(!cosmos::Simulator::has_current());
    const int plain = socket(AF_INET, SOCK_STREAM, 0);
    must(plain >= 0);
    must(close(plain) == 0);

    int sv[2] = {-1, -1};
    must(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    must(send(sv[0], "abc", 3, 0) == 3);
    char buf[4] = {};
    must(recv(sv[1], buf, sizeof(buf), 0) == 3);
    must(std::memcmp(buf, "abc", 3) == 0);
    must(close(sv[0]) == 0);
    must(close(sv[1]) == 0);
    std::cout << "[PASS] test_passthrough_without_universe" << std::endl;
}

std::string loopback_transcript(uint64_t seed) {
    cosmos::Simulator sim(seed);
    must(sim.install_faults(network_config(cosmos::SiteId::send, cosmos::FaultKind::PacketCorrupt))
             .has_value());
    cosmos::Simulator::set_current(&sim);

    Cluster pair = open_pair();
    char payload[64];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = static_cast<char>('A' + (i % 26));
    must(send(pair.client, payload, sizeof(payload), 0) == static_cast<ssize_t>(sizeof(payload)));

    struct pollfd pfd{pair.server, POLLIN, 0};
    must(poll(&pfd, 1, 0) == 1);
    must((pfd.revents & POLLIN) != 0);

    char got[64] = {};
    must(recv(pair.server, got, sizeof(got), 0) == static_cast<ssize_t>(sizeof(got)));
    drop_pair(pair);

    std::string out(got, sizeof(got));
    out += "|";
    out += std::to_string(sim.now().ns);
    cosmos::Simulator::set_current(nullptr);
    return out;
}

void test_loopback_is_byte_perfect_and_deterministic() {
    cosmos::Simulator sim(11);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    const char payload[] = "the quick brown fox";
    must(send(pair.client, payload, sizeof(payload) - 1, 0) ==
         static_cast<ssize_t>(sizeof(payload) - 1));
    char got[32] = {};
    must(recv(pair.server, got, sizeof(got), 0) == static_cast<ssize_t>(sizeof(payload) - 1));
    must(std::memcmp(got, payload, sizeof(payload) - 1) == 0);

    int sv[2] = {-1, -1};
    must(socketpair(AF_INET, SOCK_STREAM, 0, sv) == 0);
    must(sv[0] >= cosmos::kVirtualFdBase && sv[1] >= cosmos::kVirtualFdBase);
    must(send(sv[1], "pair", 4, 0) == 4);
    char back[8] = {};
    must(recv(sv[0], back, sizeof(back), 0) == 4);
    must(std::memcmp(back, "pair", 4) == 0);
    must(close(sv[0]) == 0);
    must(close(sv[1]) == 0);

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);

    must(loopback_transcript(4242) == loopback_transcript(4242));
    std::cout << "[PASS] test_loopback_is_byte_perfect_and_deterministic" << std::endl;
}

void test_poll_readiness_and_chk_alias() {
    cosmos::Simulator sim(91);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    struct pollfd out{pair.client, POLLOUT, 0};
    must(poll(&out, 1, 0) == 1);
    must((out.revents & POLLOUT) != 0);

    struct pollfd in{pair.server, POLLIN, 0};
    must(poll(&in, 1, 0) == 0);

    must(send(pair.client, "ping", 4, 0) == 4);
    in.revents = 0;
    must(poll(&in, 1, 0) == 1);
    must((in.revents & POLLIN) != 0);

    in.revents = 0;
    must(__wrap___poll_chk(&in, 1, 0, sizeof(in)) == 1);
    must((in.revents & POLLIN) != 0);

    struct pollfd empty{pair.client, POLLIN, 0};
    const cosmos::Time before = sim.now();
    must(poll(&empty, 1, 10) == 0);
    must(sim.now() >= before + cosmos::Duration{10'000'000});

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_poll_readiness_and_chk_alias" << std::endl;
}

void test_nonblocking_paths_and_fcntl() {
    cosmos::Simulator sim(71);
    cosmos::Simulator::set_current(&sim);

    uint16_t port = 0;
    const int listener_fd = bind_listener(&port, /*nonblocking=*/true);
    Address peer;
    socklen_t len = peer.size();
    errno = 0;
    must(accept(listener_fd, peer.as_mutable_sockaddr(), &len) == -1);
    must(errno == EAGAIN);

    // The ordinary way an application turns a socket non-blocking, which the transport must honour.
    const int plain = socket(AF_INET, SOCK_STREAM, 0);
    must(fcntl(plain, F_GETFL) == O_RDWR);
    must(fcntl(plain, F_SETFL, O_NONBLOCK) == 0);
    must((fcntl(plain, F_GETFL) & O_NONBLOCK) != 0);

    Cluster pair = open_pair();
    errno = 0;
    char buf[4] = {};
    must(recv(pair.server, buf, sizeof(buf), MSG_DONTWAIT) == -1);
    must(errno == EAGAIN);

    close(plain);
    drop_pair(pair);
    close(listener_fd);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_nonblocking_paths_and_fcntl" << std::endl;
}

void test_status_errors_and_close_semantics() {
    cosmos::Simulator sim(81);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    errno = 0;
    must(send(pair.listener, "x", 1, 0) == -1);
    must(errno == ENOTCONN);
    char probe[4] = {};
    errno = 0;
    must(recv(pair.listener, probe, sizeof(probe), MSG_DONTWAIT) == -1);
    must(errno == ENOTCONN);
    errno = 0;
    must(send(5000, "x", 1, 0) == -1);
    must(errno == EBADF);

    must(close(pair.server) == 0);
    pair.server = -1;
    errno = 0;
    char buf[4] = {};
    must(recv(pair.client, buf, sizeof(buf), MSG_DONTWAIT) == 0);

    must(shutdown(pair.client, SHUT_WR) == 0);
    must(close(pair.client) == 0);
    pair.client = -1;
    must(close(pair.listener) == 0);
    pair.listener = -1;
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_status_errors_and_close_semantics" << std::endl;
}

// The port table is transport-wide, so a bind must lose to any endpoint that already owns the
// port -- a listener, a bound-but-not-listening socket, or a connected client's implicit port --
// and ephemeral allocation must skip owned ports instead of wrapping onto them.
void test_port_ownership_and_ephemeral_allocation() {
    cosmos::Simulator sim(101);
    cosmos::Simulator::set_current(&sim);

    // A bound socket owns its explicit port even before it listens.
    const Address owned(45000);
    const int bound = socket(AF_INET, SOCK_STREAM, 0);
    must(bound >= 0);
    must(bind(bound, owned.as_sockaddr(), owned.size()) == 0);

    const int clash = socket(AF_INET, SOCK_STREAM, 0);
    must(clash >= 0);
    errno = 0;
    must(bind(clash, owned.as_sockaddr(), owned.size()) == -1);
    must(errno == EADDRINUSE);

    // The allocator starts at 40000; a listener parked there is exactly the collision to skip.
    const Address fixed(40000);
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    must(listener >= 0);
    must(bind(listener, fixed.as_sockaddr(), fixed.size()) == 0);
    must(listen(listener, 4) == 0);

    const Address any(0);
    const int ephemeral = socket(AF_INET, SOCK_STREAM, 0);
    must(ephemeral >= 0);
    must(bind(ephemeral, any.as_sockaddr(), any.size()) == 0);
    const uint16_t implicit_port = local_port_of(ephemeral);
    must(implicit_port != 0);
    must(implicit_port != 40000);

    const int client = connect_to(40000);
    const uint16_t client_port = local_port_of(client);
    must(client_port != 0);
    must(client_port != 40000 && client_port != implicit_port);

    // A connected client's implicit port is owned too: another socket cannot take it.
    const Address taken(client_port);
    const int thief = socket(AF_INET, SOCK_STREAM, 0);
    must(thief >= 0);
    errno = 0;
    must(bind(thief, taken.as_sockaddr(), taken.size()) == -1);
    must(errno == EADDRINUSE);

    // socketpair hands both ends distinct, non-zero ports, and each end names the other.
    int sv[2] = {-1, -1};
    must(socketpair(AF_INET, SOCK_STREAM, 0, sv) == 0);
    const uint16_t left = local_port_of(sv[0]);
    const uint16_t right = local_port_of(sv[1]);
    must(left != 0 && right != 0 && left != right);
    must(peer_port_of(sv[0]) == right);
    must(peer_port_of(sv[1]) == left);

    must(close(sv[0]) == 0);
    must(close(sv[1]) == 0);
    must(close(thief) == 0);
    must(close(client) == 0);
    must(close(ephemeral) == 0);
    must(close(listener) == 0);
    must(close(clash) == 0);
    must(close(bound) == 0);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_port_ownership_and_ephemeral_allocation" << std::endl;
}

// A closed virtual fd is not a host descriptor: poll must report POLLNVAL, immediately even for an
// infinite timeout, and must not mask a real fd polled in the same call.
void test_poll_reports_pollnval_for_a_closed_virtual_fd() {
    cosmos::Simulator sim(102);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    const int dead = socket(AF_INET, SOCK_STREAM, 0);
    must(dead >= cosmos::kVirtualFdBase);
    must(close(dead) == 0);

    struct pollfd pfd{dead, POLLIN | POLLOUT, 0};
    must(poll(&pfd, 1, 0) == 1);
    must(pfd.revents == POLLNVAL);

    // The invalid fd is ready now, so an infinite wait returns instead of parking on it.
    pfd.revents = 0;
    must(poll(&pfd, 1, -1) == 1);
    must(pfd.revents == POLLNVAL);

    // Mixed with a healthy fd that has nothing to report: only the invalid one is counted.
    struct pollfd both[2] = {{dead, POLLIN, 0}, {pair.client, POLLIN, 0}};
    must(poll(both, 2, 0) == 1);
    must(both[0].revents == POLLNVAL);
    must(both[1].revents == 0);

    // The fortified alias must agree.
    struct pollfd alias{dead, POLLIN, 0};
    must(__wrap___poll_chk(&alias, 1, 0, sizeof(alias)) == 1);
    must(alias.revents == POLLNVAL);

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_poll_reports_pollnval_for_a_closed_virtual_fd" << std::endl;
}

// MSG_DONTWAIT is the per-call non-blocking switch on send: a full peer buffer must yield EAGAIN
// from the flag alone (a blocking socket otherwise), and drained room must make the same call
// succeed.
void test_send_msg_dontwait_is_honoured() {
    cosmos::Simulator sim(103);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    char chunk[4096] = {};
    for (int i = 0; i < 16; ++i) {
        must(send(pair.client, chunk, sizeof(chunk), 0) == static_cast<ssize_t>(sizeof(chunk)));
    }

    errno = 0;
    must(send(pair.client, "x", 1, MSG_DONTWAIT) == -1);
    must(errno == EAGAIN);

    char one[1] = {};
    must(recv(pair.server, one, sizeof(one), 0) == 1);
    must(send(pair.client, "y", 1, MSG_DONTWAIT) == 1);

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_send_msg_dontwait_is_honoured" << std::endl;
}

// A peer that closes and has its slot recycled must not make the survivor talk to the impostor:
// getpeername keeps the address it was connected to, and the survivor's send is EPIPE rather than
// a write into the unrelated endpoint now occupying the peer's slot.
void test_getpeername_survives_peer_slot_recycling() {
    cosmos::Simulator sim(104);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    const int old_client = pair.client;
    const uint16_t old_client_port = local_port_of(old_client);
    must(close(pair.client) == 0);
    pair.client = -1;

    // The allocator hands out the first free slot; with the client closed that is its slot.
    const int recycled = socket(AF_INET, SOCK_STREAM, 0);
    must(recycled == old_client);

    Address peer;
    socklen_t len = peer.size();
    must(getpeername(pair.server, peer.as_mutable_sockaddr(), &len) == 0);
    must(ntohs(peer.raw.sin_port) == old_client_port);

    errno = 0;
    len = peer.size();
    must(getpeername(recycled, peer.as_mutable_sockaddr(), &len) == -1);
    must(errno == ENOTCONN);

    errno = 0;
    must(send(pair.server, "x", 1, MSG_DONTWAIT) == -1);
    must(errno == EPIPE);

    must(close(recycled) == 0);
    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_getpeername_survives_peer_slot_recycling" << std::endl;
}

// A universe must not claim sockets it does not speak: non-TCP families and DGRAM types stay with
// the host, and the file-level wrappers must fall through for their fds.
void test_host_socket_stays_host_under_a_universe() {
    cosmos::Simulator sim(105);
    cosmos::Simulator::set_current(&sim);

    const int dgram = socket(AF_INET, SOCK_DGRAM, 0);
    must(dgram >= 0);
    must(!sim.net().state(dgram).known);

    const int local = socket(AF_UNIX, SOCK_STREAM, 0);
    must(local >= 0);
    must(!sim.net().state(local).known);

    must(close(dgram) == 0);
    must(close(local) == 0);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_host_socket_stays_host_under_a_universe" << std::endl;
}

int main() {
    test_passthrough_without_universe();
    test_loopback_is_byte_perfect_and_deterministic();
    test_poll_readiness_and_chk_alias();
    test_nonblocking_paths_and_fcntl();
    test_status_errors_and_close_semantics();
    test_port_ownership_and_ephemeral_allocation();
    test_poll_reports_pollnval_for_a_closed_virtual_fd();
    test_send_msg_dontwait_is_honoured();
    test_getpeername_survives_peer_slot_recycling();
    test_host_socket_stays_host_under_a_universe();
    std::cout << "All network transport tests passed successfully!" << std::endl;
    return 0;
}
