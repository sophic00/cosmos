#include "net_test_support.hpp"

// White-box include: pins the internal network eligibility policy, not just the public behavior.
#include "wrapper_fault.hpp"

#include <cstdio>
#include <iostream>
#include <poll.h>

extern "C" ssize_t __wrap___recv_chk(int fd, void* buf, size_t len, size_t buflen, int flags);

// The transport speaks IPv4 only, and these predicates decide whether the injector is consulted.
static_assert(!cosmos::wrappers::network_addr_eligible(nullptr, 0));
static_assert(cosmos::wrappers::network_accept_eligible(true));
static_assert(!cosmos::wrappers::network_accept_eligible(false));
static_assert(cosmos::wrappers::network_send_eligible(true, 1));
static_assert(!cosmos::wrappers::network_send_eligible(true, 0));
static_assert(!cosmos::wrappers::network_send_eligible(false, 8));
static_assert(cosmos::wrappers::network_recv_eligible(true, 1));
static_assert(!cosmos::wrappers::network_recv_eligible(true, 0));
static_assert(!cosmos::wrappers::network_recv_eligible(false, 8));

void test_fault_free_connect_is_refused_without_a_listener() {
    cosmos::Simulator sim(12);
    // Armed but unable to fire: the refusal below must come from the transport, not the injector.
    must(sim.install_faults(
                network_config(cosmos::SiteId::connect, cosmos::FaultKind::ConnRefused, {}, 0.0))
             .has_value());
    cosmos::Simulator::set_current(&sim);

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    must(fd >= 0);
    const Address nowhere(39999);
    errno = 0;
    must(connect(fd, nowhere.as_sockaddr(), nowhere.size()) == -1);
    must(errno == ECONNREFUSED);
    must(sim.injector_or_null()->injections(cosmos::SiteId::connect) == 0);
    must(sim.injector_or_null()->eligible_calls(cosmos::SiteId::connect) == 1);
    must(close(fd) == 0);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_fault_free_connect_is_refused_without_a_listener" << std::endl;
}

void test_connect_outcomes() {
    struct Case {
        cosmos::FaultKind kind;
        int error;
    };
    const Case cases[] = {
        {cosmos::FaultKind::ConnRefused, ECONNREFUSED},
        // connect(2) has no ECONNRESET, so a mid-handshake reset reports its legal neighbour.
        {cosmos::FaultKind::ConnReset, ETIMEDOUT},
    };
    uint64_t seed = 21;
    for (const Case& c : cases) {
        cosmos::Simulator sim(seed++);
        must(sim.install_faults(network_config(cosmos::SiteId::connect, c.kind)).has_value());
        cosmos::Simulator::set_current(&sim);

        uint16_t port = 0;
        const int listener_fd = bind_listener(&port);
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        const Address addr(port);
        errno = 0;
        must(connect(fd, addr.as_sockaddr(), addr.size()) == -1);
        must(errno == c.error);
        must(sim.injector_or_null()->eligible_calls(cosmos::SiteId::connect) == 1);
        must(sim.injector_or_null()->injections(cosmos::SiteId::connect) == 1);
        close(fd);
        close(listener_fd);
        cosmos::Simulator::set_current(nullptr);
    }
    std::cout << "[PASS] test_connect_outcomes" << std::endl;
}

void test_accept_outcome_drops_the_queued_connection() {
    cosmos::Simulator sim(31);
    must(sim.install_faults(network_config(cosmos::SiteId::accept, cosmos::FaultKind::ConnReset))
             .has_value());
    cosmos::Simulator::set_current(&sim);

    uint16_t port = 0;
    const int listener_fd = bind_listener(&port, /*nonblocking=*/true);
    const int client = connect_to(port);
    Address peer;
    socklen_t len = peer.size();
    errno = 0;
    must(accept(listener_fd, peer.as_mutable_sockaddr(), &len) == -1);
    // accept(2) lists ECONNABORTED and not ECONNRESET; the queued connection is gone afterwards.
    must(errno == ECONNABORTED);
    must(sim.injector_or_null()->eligible_calls(cosmos::SiteId::accept) == 1);
    must(sim.injector_or_null()->injections(cosmos::SiteId::accept) == 1);

    errno = 0;
    must(accept(listener_fd, peer.as_mutable_sockaddr(), &len) == -1);
    must(errno == EAGAIN);
    must(sim.injector_or_null()->eligible_calls(cosmos::SiteId::accept) == 1);

    close(client);
    close(listener_fd);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_accept_outcome_drops_the_queued_connection" << std::endl;
}

void test_send_outcomes() {
    const char payload[] = "abcdefgh";

    {
        cosmos::Simulator sim(41);
        must(sim.install_faults(network_config(cosmos::SiteId::send, cosmos::FaultKind::ConnReset))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        errno = 0;
        must(send(pair.client, payload, 8, 0) == -1);
        must(errno == ECONNRESET);
        must(sim.injector_or_null()->eligible_calls(cosmos::SiteId::send) == 1);
        must(sim.injector_or_null()->injections(cosmos::SiteId::send) == 1);
        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        cosmos::Simulator sim(42);
        must(sim.install_faults(network_config(cosmos::SiteId::send, cosmos::FaultKind::ShortSend))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        must(send(pair.client, payload, 8, 0) == 4);
        must(receive(pair.server) == 4);
        errno = 0;
        must(recv(pair.server, nullptr, 0, MSG_DONTWAIT) == 0);
        char got[16] = {};
        must(recv(pair.server, got, sizeof(got), MSG_DONTWAIT) == -1);
        must(errno == EAGAIN);
        // A 1-byte send has no legal short observable, so it degrades to a complete transfer.
        must(send(pair.client, "x", 1, 0) == 1);
        must(receive(pair.server) == 1);
        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        cosmos::Simulator sim(43);
        must(sim.install_faults(network_config(cosmos::SiteId::send, cosmos::FaultKind::PacketDrop))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        must(send(pair.client, payload, 8, 0) == 8);
        errno = 0;
        must(receive(pair.server) == -1);
        must(errno == EAGAIN);
        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        cosmos::Simulator sim(44);
        must(sim.install_faults(
                    network_config(cosmos::SiteId::send, cosmos::FaultKind::PacketCorrupt))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        must(send(pair.client, payload, 8, 0) == 8);
        char got[8] = {};
        must(recv(pair.server, got, sizeof(got), 0) == 8);
        for (size_t i = 0; i < sizeof(got); ++i) {
            must(got[i] == static_cast<char>(payload[i] ^ 0x01));
        }
        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        cosmos::Simulator sim(45);
        must(sim.install_faults(
                    network_config(cosmos::SiteId::send, cosmos::FaultKind::PacketReorder))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        must(send(pair.client, "FIRST", 5, 0) == 5);
        must(send(pair.client, "SECND", 5, 0) == 5);
        char got[6] = {};
        must(recv(pair.server, got, sizeof(got), 0) == 5);
        must(std::memcmp(got, "SECND", 5) == 0);
        must(recv(pair.server, got, sizeof(got), 0) == 5);
        must(std::memcmp(got, "FIRST", 5) == 0);
        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }
    std::cout << "[PASS] test_send_outcomes" << std::endl;
}

void test_recv_outcomes_and_delay() {
    {
        cosmos::Simulator sim(51);
        must(sim.install_faults(network_config(cosmos::SiteId::recv, cosmos::FaultKind::ConnReset))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        must(recv(pair.server, nullptr, 0, 0) == 0);
        must(sim.injector_or_null()->eligible_calls(cosmos::SiteId::recv) == 0);
        char buf[8] = {};
        errno = 0;
        must(recv(pair.server, buf, sizeof(buf), 0) == -1);
        must(errno == ECONNRESET);
        must(sim.injector_or_null()->injections(cosmos::SiteId::recv) == 1);
        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        cosmos::Simulator sim(52);
        must(sim.install_faults(network_config(cosmos::SiteId::recv, cosmos::FaultKind::PeerClose))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        char buf[8] = {};
        must(recv(pair.server, buf, sizeof(buf), 0) == 0);
        must(recv(pair.server, buf, sizeof(buf), MSG_DONTWAIT) == 0);
        // Data already buffered is delivered before the EOF, as a real socket does.
        must(send(pair.client, "tail", 4, 0) == 4);
        must(recv(pair.server, buf, sizeof(buf), 0) == 4);
        must(std::memcmp(buf, "tail", 4) == 0);
        must(recv(pair.server, buf, sizeof(buf), 0) == 0);
        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        cosmos::Simulator sim(53);
        must(sim.install_faults(network_config(cosmos::SiteId::send, cosmos::FaultKind::PacketDelay,
                                               cosmos::Duration{5'000'000}))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        must(send(pair.client, "late", 4, 0) == 4);
        char buf[8] = {};
        errno = 0;
        must(recv(pair.server, buf, sizeof(buf), MSG_DONTWAIT) == -1);
        must(errno == EAGAIN);
        must(recv(pair.server, buf, sizeof(buf), 0) == 4);
        must(std::memcmp(buf, "late", 4) == 0);
        must(sim.now() >= cosmos::Time::zero() + cosmos::Duration{5'000'000});
        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }
    std::cout << "[PASS] test_recv_outcomes_and_delay" << std::endl;
}

void test_blocking_recv_suspends_the_fiber() {
    cosmos::Simulator sim(61);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    Exchange exchange;
    exchange.fd = pair.server;
    pthread_t reader = 0;
    must(pthread_create(&reader, nullptr, blocked_reader, &exchange) == 0);
    for (int i = 0; i < 16 && !exchange.entered; ++i)
        sched_yield();
    sched_yield();
    must(exchange.entered);
    must(exchange.got == -1);

    must(send(pair.client, "wake", 4, 0) == 4);
    void* retval = nullptr;
    must(pthread_join(reader, &retval) == 0);
    must(exchange.got == 4);
    must(std::memcmp(exchange.bytes, "wake", 4) == 0);

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_blocking_recv_suspends_the_fiber" << std::endl;
}

void test_blocked_recv_without_a_sender_reports_deadlock() {
    cosmos::Simulator sim(62);
    int sv[2] = {-1, -1};
    cosmos::Simulator::set_current(&sim);
    must(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    Exchange exchange;
    exchange.fd = sv[0];
    pthread_t reader = 0;
    must(pthread_create(&reader, nullptr, blocked_reader, &exchange) == 0);
    sim.scheduler().run_until_quiescence();
    must(exchange.entered);
    must(sim.scheduler().deadlocked());
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_blocked_recv_without_a_sender_reports_deadlock" << std::endl;
}

// Called directly because whether the compiler emits the fortified alias is a property of the
// build: a plain one does not, and -fsanitize=address does.
namespace {

struct BlockedSend {
    int fd = -1;
    ssize_t sent = -1;
    bool entered = false;
};

void* blocking_sender(void* arg) {
    auto* s = static_cast<BlockedSend*>(arg);
    s->entered = true;
    s->sent = send(s->fd, "z", 1, 0);
    return nullptr;
}

} // namespace

void test_partial_drain_wakes_a_blocked_sender() {
    cosmos::Simulator sim(111);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    char chunk[4096] = {};
    for (int i = 0; i < 16; ++i) {
        must(send(pair.client, chunk, sizeof(chunk), 0) == static_cast<ssize_t>(sizeof(chunk)));
    }

    // Exactly full: not writable, because a 1-byte send would still have to wait for room.
    struct pollfd pfd{pair.client, POLLOUT, 0};
    must(poll(&pfd, 1, 0) == 0);

    BlockedSend blocked;
    blocked.fd = pair.client;
    pthread_t sender = 0;
    must(pthread_create(&sender, nullptr, blocking_sender, &blocked) == 0);
    for (int i = 0; i < 16 && !blocked.entered; ++i)
        sched_yield();
    sched_yield();
    must(blocked.entered);
    must(blocked.sent == -1);

    // One byte drained is one byte of room, and that alone must wake the parked sender; without the
    // wake this join blocks forever and the drain reports a deadlock a real kernel would not have.
    char one[1] = {};
    must(recv(pair.server, one, sizeof(one), 0) == 1);

    void* retval = nullptr;
    must(pthread_join(sender, &retval) == 0);
    must(blocked.sent == 1);

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_partial_drain_wakes_a_blocked_sender" << std::endl;
}

void test_fortified_recv_alias_reaches_the_same_site() {
    cosmos::Simulator sim(101);
    must(sim.install_faults(network_config(cosmos::SiteId::recv, cosmos::FaultKind::ConnReset))
             .has_value());
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    char buf[8] = {};
    errno = 0;
    must(__wrap___recv_chk(pair.server, buf, sizeof(buf), sizeof(buf), 0) == -1);
    must(errno == ECONNRESET);
    must(sim.injector_or_null()->injections(cosmos::SiteId::recv) == 1);

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);

    int sv[2] = {-1, -1};
    must(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    must(send(sv[0], "back", 4, 0) == 4);
    must(__wrap___recv_chk(sv[1], buf, sizeof(buf), sizeof(buf), 0) == 4);
    must(std::memcmp(buf, "back", 4) == 0);
    must(close(sv[0]) == 0);
    must(close(sv[1]) == 0);
    std::cout << "[PASS] test_fortified_recv_alias_reaches_the_same_site" << std::endl;
}

void test_unactivated_network_site_never_moves_the_stream() {
    constexpr uint64_t kSeed = 0x9E7;
    cosmos::Simulator sim(kSeed);
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Network);
    must(cfg.activate_site(cosmos::SiteId::send));
    cosmos::FaultRule rule;
    rule.rate = 0.5;
    must(rule.outcomes.add(cosmos::FaultKind::ConnReset, 1.0));
    must(cfg.set_rule(cosmos::SiteId::send, rule));
    must(sim.install_faults(std::move(cfg)).has_value());
    cosmos::Simulator::set_current(&sim);

    cosmos::Rng reference(cosmos::fault_class_seed(
        cosmos::stream_seed(kSeed, cosmos::StreamDomain::Fault), cosmos::FaultClass::Network));

    Cluster pair = open_pair(/*listener_nonblocking=*/true);
    uint64_t fired = 0;
    for (int i = 0; i < 40; ++i) {
        const bool expect_fire = reference.uniform() < 0.5;
        errno = 0;
        const ssize_t sent = send(pair.client, "abcd", 4, 0);
        if (expect_fire) {
            must(sent == -1);
            must(errno == ECONNRESET);
            ++fired;
        } else {
            must(sent == 4);
        }
    }

    must(recv(pair.server, nullptr, 0, 0) == 0);
    must(send(pair.client, nullptr, 0, 0) == 0);
    Address peer;
    socklen_t peer_len = peer.size();
    errno = 0;
    must(accept(pair.listener, peer.as_mutable_sockaddr(), &peer_len) == -1);
    must(errno == EAGAIN);

    must(fired > 0 && fired < 40);
    must(sim.injector_or_null()->eligible_calls(cosmos::SiteId::send) == 40);
    must(sim.injector_or_null()->injections(cosmos::SiteId::send) == fired);
    const auto* injector = sim.injector_or_null();
    must(injector->eligible_calls(cosmos::SiteId::recv) == 0);
    must(injector->eligible_calls(cosmos::SiteId::connect) == 0);
    must(injector->eligible_calls(cosmos::SiteId::accept) == 0);

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_unactivated_network_site_never_moves_the_stream" << std::endl;
}

// The finite-timeout poll path wakes at the earlier of the deadline and the next delayed
// delivery, so a PacketDelay neither burns the whole timeout nor parks forever on an infinite one.
void test_poll_wakes_at_the_delayed_delivery() {
    {
        cosmos::Simulator sim(71);
        must(sim.install_faults(network_config(cosmos::SiteId::send, cosmos::FaultKind::PacketDelay,
                                               cosmos::Duration{5'000'000}))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        must(send(pair.client, "late", 4, 0) == 4);

        struct pollfd pfd{pair.server, POLLIN, 0};
        must(poll(&pfd, 1, 10) == 1);
        must((pfd.revents & POLLIN) != 0);
        must(sim.now().ns == 5'000'000); // delivery, not the 10ms deadline

        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        // A delay beyond the timeout must not push the wait past the deadline.
        cosmos::Simulator sim(72);
        must(sim.install_faults(network_config(cosmos::SiteId::send, cosmos::FaultKind::PacketDelay,
                                               cosmos::Duration{5'000'000}))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        must(send(pair.client, "late", 4, 0) == 4);

        struct pollfd pfd{pair.server, POLLIN, 0};
        must(poll(&pfd, 1, 2) == 0);
        must((pfd.revents & POLLIN) == 0);
        must(sim.now().ns == 2'000'000);

        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        // An infinite wait must land on the delivery instead of parking on a packet that has no
        // broadcast left to wake it.
        cosmos::Simulator sim(73);
        must(sim.install_faults(network_config(cosmos::SiteId::send, cosmos::FaultKind::PacketDelay,
                                               cosmos::Duration{5'000'000}))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        Cluster pair = open_pair();
        must(send(pair.client, "late", 4, 0) == 4);

        struct pollfd pfd{pair.server, POLLIN, 0};
        must(poll(&pfd, 1, -1) == 1);
        must((pfd.revents & POLLIN) != 0);
        must(sim.now().ns == 5'000'000);

        drop_pair(pair);
        cosmos::Simulator::set_current(nullptr);
    }

    std::cout << "[PASS] test_poll_wakes_at_the_delayed_delivery" << std::endl;
}

int main() {
    test_fault_free_connect_is_refused_without_a_listener();
    test_connect_outcomes();
    test_accept_outcome_drops_the_queued_connection();
    test_send_outcomes();
    test_recv_outcomes_and_delay();
    test_blocking_recv_suspends_the_fiber();
    test_blocked_recv_without_a_sender_reports_deadlock();
    test_partial_drain_wakes_a_blocked_sender();
    test_fortified_recv_alias_reaches_the_same_site();
    test_unactivated_network_site_never_moves_the_stream();
    test_poll_wakes_at_the_delayed_delivery();
    std::cout << "All network fault tests passed successfully!" << std::endl;
    return 0;
}
