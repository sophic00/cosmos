#include "cosmos/cosmos.hpp"

// White-box include: this test pins the internal storage eligibility policy, not just the
// public behavior. See the target_include_directories entry in tests/CMakeLists.txt.
#include "wrapper_fault.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Every kind a storage wrapper can emit must stay legal for its site (Rule 15); a taxonomy
// change that breaks one of these is a compile error here, not a false positive in a campaign.
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::open, cosmos::FaultKind::OpenEio));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::open, cosmos::FaultKind::NoSpace));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::read, cosmos::FaultKind::ReadEio));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::write, cosmos::FaultKind::WriteEio));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::write, cosmos::FaultKind::ShortWrite));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::write, cosmos::FaultKind::NoSpace));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::fsync, cosmos::FaultKind::FsyncEio));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::fsync, cosmos::FaultKind::NoSpace));
// Nothing else is: spot-check the cross-site borrowings validate() must reject.
static_assert(!cosmos::is_legal_outcome(cosmos::SiteId::read, cosmos::FaultKind::NoSpace));
static_assert(!cosmos::is_legal_outcome(cosmos::SiteId::open, cosmos::FaultKind::ReadEio));

// Eligibility policy, pinned at compile time (white-box). Without an injector the wrapper's
// decision path folds to None, so only these static asserts can prove the policy itself;
// end-to-end eligibility through a live injector lands with F2.
static_assert(!cosmos::wrappers::storage_fd_eligible(0));
static_assert(!cosmos::wrappers::storage_fd_eligible(1));
static_assert(!cosmos::wrappers::storage_fd_eligible(2));
static_assert(!cosmos::wrappers::storage_fd_eligible(-1));
static_assert(cosmos::wrappers::storage_fd_eligible(3));
static_assert(!cosmos::wrappers::storage_write_eligible(3, 0)); // empty transfer
static_assert(cosmos::wrappers::storage_write_eligible(3, 1));  // 1-byte writes stay eligible
static_assert(cosmos::wrappers::storage_write_eligible(3, 4096));
static_assert(!cosmos::wrappers::storage_write_eligible(1, 4096)); // std stream fd
static_assert(!cosmos::wrappers::storage_read_eligible(3, 0));
static_assert(cosmos::wrappers::storage_read_eligible(3, 128));
static_assert(!cosmos::wrappers::storage_read_eligible(2, 128)); // std stream fd

// Called directly, the way the __read_chk test does: the alias must reach the open site whether
// or not this translation unit was fortified.
extern "C" int __wrap___open_2(const char* pathname, int flags);
extern "C" int __wrap___open64_2(const char* pathname, int flags);

namespace {

std::string g_temp_dir;

std::string temp_path(const char* tag) {
    static int counter = 0;
    return g_temp_dir + "/cosmos_storage_" + tag + "_" + std::to_string(getpid()) + "_" +
           std::to_string(counter++);
}

void must(bool ok) { assert(ok); }

cosmos::FaultConfig storage_config(cosmos::SiteId site, cosmos::FaultKind kind) {
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Storage);
    must(cfg.activate_site(site));
    cosmos::FaultRule rule;
    rule.rate = 1.0;
    must(rule.outcomes.add(kind, 1.0));
    must(cfg.set_rule(site, rule));
    return cfg;
}

int open_scratch(const std::string& path) {
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    must(fd >= 3);
    return fd;
}

} // namespace

void test_passthrough_no_sim() {
    assert(!cosmos::Simulator::has_current());

    const std::string path = temp_path("passthrough");
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    const char msg[] = "hello cosmos";
    assert(write(fd, msg, sizeof(msg)) == (ssize_t)sizeof(msg));
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);

    fd = open(path.c_str(), O_RDONLY);
    assert(fd >= 0);
    char buf[64] = {};
    assert(read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(msg));
    assert(strcmp(buf, msg) == 0);
    assert(close(fd) == 0);
    unlink(path.c_str());

    std::cout << "[PASS] test_passthrough_no_sim" << std::endl;
}

// With no rule installed the decision folds to None: a full round-trip inside an active Simulator
// behaves exactly like the host, and successes never clobber errno.
void test_value_path_under_sim() {
    const std::string path = temp_path("value");

    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    const char msg[] = "deterministic bytes";
    const size_t len = sizeof(msg) - 1;
    assert(write(fd, msg, len) == (ssize_t)len);
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);

    fd = open(path.c_str(), O_RDONLY);
    assert(fd >= 0);
    char buf[64] = {};
    assert(read(fd, buf, sizeof(buf)) == (ssize_t)len);
    assert(strcmp(buf, msg) == 0);
    assert(close(fd) == 0);
    assert(errno == 0);
    unlink(path.c_str());

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_value_path_under_sim" << std::endl;
}

// Zero-length I/O is never eligible: the real call answers, including on odd fds.
// 1-byte writes are eligible (they can legally fail at F2) and pass through today.
void test_zero_length_io_passthrough() {
    const std::string path = temp_path("tiny");

    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    const char buf[] = {'a', 'b', 'c', 'd'};
    assert(write(fd, buf, 0) == 0); // zero-length: passthrough, no decision
    assert(write(fd, buf, 1) == 1); // 1-byte: eligible, honest full write today

    char in[8] = {};
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(read(fd, in, sizeof(in)) == 1); // exactly the one written byte landed
    assert(in[0] == 'a');
    assert(read(fd, in, 0) == 0); // zero-length read: passthrough
    assert(close(fd) == 0);
    assert(errno == 0);
    unlink(path.c_str());

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_zero_length_io_passthrough" << std::endl;
}

// Standard stream fds are never eligible: logging must stay draw-free and unfaulted once a
// real injector is wired (F2). Today this proves the passthrough is intact on fds 1 and 2.
void test_std_stream_fds_pass_through() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    const char probe[] = "(cosmos: std-stream passthrough probe)\n";
    assert(write(1, probe, sizeof(probe) - 1) == (ssize_t)(sizeof(probe) - 1));
    assert(write(2, probe, sizeof(probe) - 1) == (ssize_t)(sizeof(probe) - 1));
    assert(errno == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_std_stream_fds_pass_through" << std::endl;
}

// The variadic mode argument must survive the wrapper on both paths: O_CREAT files carry the
// requested permissions (masked by the process umask, exactly like the real call).
void test_open_mode_forwarding() {
    const mode_t mask = umask(0);
    umask(mask);
    const mode_t expect = static_cast<mode_t>(0640 & ~mask);

    const std::string host_path = temp_path("mode_host");
    int fd = open(host_path.c_str(), O_CREAT | O_WRONLY, 0640);
    assert(fd >= 0);
    struct stat st{};
    assert(fstat(fd, &st) == 0);
    assert((st.st_mode & 0777) == expect);
    assert(close(fd) == 0);
    unlink(host_path.c_str());

    const std::string sim_path = temp_path("mode_sim");
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);
    fd = open(sim_path.c_str(), O_CREAT | O_WRONLY, 0640);
    assert(fd >= 0);
    assert(fstat(fd, &st) == 0);
    assert((st.st_mode & 0777) == expect);
    assert(close(fd) == 0);
    unlink(sim_path.c_str());
    cosmos::Simulator::set_current(nullptr);

    std::cout << "[PASS] test_open_mode_forwarding" << std::endl;
}

// The wrapper path must not allocate (Rule 7): with malloc wrapped, any internal allocation
// while the sim is current would land on the tracked heap and trip the counters.
void test_no_alloc_smoke() {
    const std::string path = temp_path("alloc");

    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[16] = {};
    for (int i = 0; i < 10000; ++i) {
        assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
        assert(lseek(fd, 0, SEEK_SET) == 0);
        assert(read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
    }
    for (int i = 0; i < 100; ++i) {
        assert(fsync(fd) == 0);
    }
    assert(close(fd) == 0);
    unlink(path.c_str());

    assert(sim.heap().stats().active_allocations == 0);
    assert(sim.heap().stats().total_allocation_count == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_no_alloc_smoke" << std::endl;
}

// glibc substitutes __read_chk for read() wherever the destination size is known and fortification
// is on, so the alias has to reach the same site or the wrapper has a hole nobody notices. Called
// directly because whether the compiler emits it depends on the build: -fsanitize=address does,
// a plain build does not.
extern "C" ssize_t __wrap___read_chk(int fd, void* buf, size_t count, size_t buflen);

void test_fortified_read_alias_reaches_the_same_site() {
    const std::string path = temp_path("readchk");
    int fd = open_scratch(path);
    assert(::write(fd, "0123456789", 10) == 10);
    assert(lseek(fd, 0, SEEK_SET) == 0);

    char buf[10];
    {
        cosmos::Simulator sim(7);
        must(sim.install_faults(storage_config(cosmos::SiteId::read, cosmos::FaultKind::ReadEio))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        errno = 0;
        assert(__wrap___read_chk(fd, buf, sizeof(buf), sizeof(buf)) == -1);
        assert(errno == EIO);
        assert(sim.injector_or_null()->injections(cosmos::SiteId::read) == 1);
        cosmos::Simulator::set_current(nullptr);
    }

    // Same alias, no universe: the host answers and the data is intact.
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(__wrap___read_chk(fd, buf, sizeof(buf), sizeof(buf)) == 10);
    assert(std::memcmp(buf, "0123456789", 10) == 0);

    close(fd);
    unlink(path.c_str());
    std::cout << "[PASS] test_fortified_read_alias_reaches_the_same_site" << std::endl;
}

// glibc substitutes __open_2/__open64_2 for a two-argument open() under _FORTIFY_SOURCE, and
// --wrap=open does not see those symbols. Both must reach the same site, so a fortified build
// cannot silently skip it.
void test_fortified_open_aliases_reach_the_same_site() {
    const std::string path = temp_path("open2");
    {
        cosmos::Simulator sim(11);
        must(sim.install_faults(storage_config(cosmos::SiteId::open, cosmos::FaultKind::OpenEio))
                 .has_value());
        cosmos::Simulator::set_current(&sim);

        errno = 0;
        assert(__wrap___open_2(path.c_str(), O_RDWR) == -1);
        assert(errno == EIO);
        errno = 0;
        assert(__wrap___open64_2(path.c_str(), O_RDWR) == -1);
        assert(errno == EIO);
        assert(sim.injector_or_null()->injections(cosmos::SiteId::open) == 2);
        assert(sim.injector_or_null()->eligible_calls(cosmos::SiteId::open) == 2);

        cosmos::Simulator::set_current(nullptr);
    }

    // No universe: the alias reaches the host. Creation is done with the ordinary open, because
    // the fortified two-argument form cannot carry a mode.
    int fd = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    assert(fd >= 3);
    assert(close(fd) == 0);
    fd = __wrap___open_2(path.c_str(), O_RDWR);
    assert(fd >= 3);
    assert(close(fd) == 0);
    unlink(path.c_str());

    std::cout << "[PASS] test_fortified_open_aliases_reach_the_same_site" << std::endl;
}

// The decision precedes the real call, so a faulted call must leave the filesystem as it found it:
// nothing written, nothing created.
void test_faulted_calls_leave_no_side_effects() {
    const std::string path = temp_path("sideeffect");

    {
        cosmos::Simulator sim(17);
        must(sim.install_faults(storage_config(cosmos::SiteId::open, cosmos::FaultKind::OpenEio))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        assert(open(path.c_str(), O_CREAT | O_RDWR, 0600) == -1);
        cosmos::Simulator::set_current(nullptr);
    }
    assert(access(path.c_str(), F_OK) != 0); // the faulted open created nothing

    int fd = open_scratch(path);
    assert(::write(fd, "original-content", 16) == 16);

    {
        cosmos::Simulator sim(18);
        must(sim.install_faults(storage_config(cosmos::SiteId::write, cosmos::FaultKind::WriteEio))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        assert(::write(fd, "clobbered", 9) == -1);
        assert(sim.injector_or_null()->injections(cosmos::SiteId::write) == 1);
        cosmos::Simulator::set_current(nullptr);
    }

    assert(lseek(fd, 0, SEEK_SET) == 0);
    char buf[32] = {};
    assert(::read(fd, buf, sizeof(buf)) == 16);
    assert(memcmp(buf, "original-content", 16) == 0); // the faulted write never reached the file

    close(fd);
    unlink(path.c_str());
    std::cout << "[PASS] test_faulted_calls_leave_no_side_effects" << std::endl;
}

// Fds the storage surface does not own must not spend Storage draws: an invalid number and a
// cosmos virtual socket fd (which the file API cannot fault at all).
void test_ineligible_fds_never_move_the_storage_stream() {
    int sockets[2] = {0, 0};
    {
        cosmos::Simulator sim(19);
        must(sim.install_faults(storage_config(cosmos::SiteId::write, cosmos::FaultKind::WriteEio))
                 .has_value());
        cosmos::Simulator::set_current(&sim);

        assert(sim.net().pair(AF_INET, SOCK_STREAM, 0, sockets) == 0);
        assert(sockets[0] >= cosmos::kVirtualFdBase);

        // Both calls reach the host and fail there. What this test owns is that the storage
        // surface left them alone: routing a socket fd to the transport is a separate gap,
        // recorded in the plan rather than papered over with a fault.
        const ssize_t on_a_socket = ::write(sockets[0], "x", 1);
        const ssize_t on_a_bad_fd = ::write(-1, "x", 1);
        (void)on_a_socket;
        (void)on_a_bad_fd;

        auto* injector = sim.injector_or_null();
        assert(injector->eligible_calls(cosmos::SiteId::write) == 0);
        assert(injector->injections(cosmos::SiteId::write) == 0);

        cosmos::Simulator::set_current(nullptr);
    }

    std::cout << "[PASS] test_ineligible_fds_never_move_the_storage_stream" << std::endl;
}

// Subset of test_storage_outcome_matrix: kept as a smoke path, not as errno coverage.
void test_storage_outcomes_map_to_errno() {
    const std::string path = temp_path("errno");

    {
        cosmos::Simulator sim(1);
        must(sim.install_faults(storage_config(cosmos::SiteId::open, cosmos::FaultKind::OpenEio))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        errno = 0;
        assert(open(path.c_str(), O_CREAT | O_RDWR, 0600) == -1);
        assert(errno == EIO);
        assert(sim.injector_or_null()->injections(cosmos::SiteId::open) == 1);
        cosmos::Simulator::set_current(nullptr);
    }

    int fd = open_scratch(path);
    assert(::write(fd, "0123456789", 10) == 10);
    assert(lseek(fd, 0, SEEK_SET) == 0);

    {
        cosmos::Simulator sim(2);
        must(sim.install_faults(storage_config(cosmos::SiteId::read, cosmos::FaultKind::ReadEio))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        char buf[10];
        errno = 0;
        assert(::read(fd, buf, sizeof(buf)) == -1);
        assert(errno == EIO);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        cosmos::Simulator sim(3);
        must(sim.install_faults(storage_config(cosmos::SiteId::write, cosmos::FaultKind::NoSpace))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        errno = 0;
        assert(::write(fd, "xxxx", 4) == -1);
        assert(errno == ENOSPC);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        cosmos::Simulator sim(4);
        must(
            sim.install_faults(storage_config(cosmos::SiteId::write, cosmos::FaultKind::ShortWrite))
                .has_value());
        cosmos::Simulator::set_current(&sim);
        // ShortWrite is a shaped success: the bytes it reports must actually have been written.
        assert(::write(fd, "abcdefgh", 8) == 4);
        cosmos::Simulator::set_current(nullptr);
    }

    {
        cosmos::Simulator sim(5);
        must(sim.install_faults(storage_config(cosmos::SiteId::fsync, cosmos::FaultKind::FsyncEio))
                 .has_value());
        cosmos::Simulator::set_current(&sim);
        errno = 0;
        assert(fsync(fd) == -1);
        assert(errno == EIO);
        cosmos::Simulator::set_current(nullptr);
    }

    close(fd);
    unlink(path.c_str());
    std::cout << "[PASS] test_storage_outcomes_map_to_errno" << std::endl;
}

// Rule 3 through the wrapper: an unactivated site must not move its siblings' class sub-stream.
void test_unactivated_site_never_moves_the_storage_stream() {
    constexpr uint64_t kSeed = 0x5709A6E;
    const std::string path = temp_path("rule3");
    int fd = open_scratch(path);

    cosmos::Simulator sim(kSeed);
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Storage);
    must(cfg.activate_site(cosmos::SiteId::write));
    cosmos::FaultRule rule;
    rule.rate = 0.5;
    must(rule.outcomes.add(cosmos::FaultKind::WriteEio, 1.0));
    must(cfg.set_rule(cosmos::SiteId::write, rule));
    must(sim.install_faults(std::move(cfg)).has_value());
    cosmos::Simulator::set_current(&sim);

    cosmos::Rng reference(cosmos::fault_class_seed(
        cosmos::stream_seed(kSeed, cosmos::StreamDomain::Fault), cosmos::FaultClass::Storage));

    char buf[4];
    uint64_t fired = 0;
    for (int i = 0; i < 60; ++i) {
        assert(lseek(fd, 0, SEEK_SET) == 0);
        assert(::read(fd, buf, sizeof(buf)) >= 0);

        const bool expect_fire = reference.uniform() < 0.5;
        errno = 0;
        const ssize_t written = ::write(fd, "abcd", 4);
        if (expect_fire) {
            assert(written == -1);
            assert(errno == EIO);
            ++fired;
        } else {
            assert(written == 4);
        }
        assert(fsync(fd) == 0);
        const int other = ::open(path.c_str(), O_RDWR);
        assert(other >= 0);
        assert(::close(other) == 0);
    }
    assert(fired > 0 && fired < 60);
    assert(sim.injector_or_null()->eligible_calls(cosmos::SiteId::read) == 0);
    assert(sim.injector_or_null()->eligible_calls(cosmos::SiteId::fsync) == 0);
    assert(sim.injector_or_null()->eligible_calls(cosmos::SiteId::open) == 0);
    assert(sim.injector_or_null()->injections(cosmos::SiteId::write) == fired);

    cosmos::Simulator::set_current(nullptr);
    close(fd);
    unlink(path.c_str());
    std::cout << "[PASS] test_unactivated_site_never_moves_the_storage_stream" << std::endl;
}

// Rule 7, and the property the merged reentrancy guard buys across wrapper families.
void test_allocation_inside_wrapper_logic_is_never_faulted() {
    cosmos::Simulator sim(13);
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Memory);
    must(cfg.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = 1.0;
    must(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    must(cfg.set_rule(cosmos::SiteId::malloc, rule));
    must(sim.install_faults(std::move(cfg)).has_value());
    cosmos::Simulator::set_current(&sim);

    {
        cosmos::wrappers::ReentrancyGuard guard;
        void* p = malloc(128);
        assert(p != nullptr);
        free(p);
    }
    assert(sim.injector_or_null()->eligible_calls(cosmos::SiteId::malloc) == 0);

    assert(malloc(128) == nullptr);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_allocation_inside_wrapper_logic_is_never_faulted" << std::endl;
}

struct MatrixCase {
    cosmos::SiteId site;
    cosmos::FaultKind kind;
    size_t count;
    ssize_t result;
    int error;
};

// Rule 15 at the wire: the errno an injected outcome produces must be one the man page lists.
bool listed_errno(cosmos::SiteId site, int error) {
    switch (site) {
    case cosmos::SiteId::open:
    case cosmos::SiteId::write:
    case cosmos::SiteId::fsync:
        return error == EIO || error == ENOSPC;
    case cosmos::SiteId::read:
        return error == EIO;
    default:
        return false;
    }
}

void test_storage_outcome_matrix() {
    static const MatrixCase cases[] = {
        {cosmos::SiteId::open, cosmos::FaultKind::OpenEio, 0, -1, EIO},
        {cosmos::SiteId::open, cosmos::FaultKind::NoSpace, 0, -1, ENOSPC},
        {cosmos::SiteId::read, cosmos::FaultKind::ReadEio, 4, -1, EIO},
        {cosmos::SiteId::write, cosmos::FaultKind::WriteEio, 4, -1, EIO},
        {cosmos::SiteId::write, cosmos::FaultKind::NoSpace, 4, -1, ENOSPC},
        {cosmos::SiteId::write, cosmos::FaultKind::ShortWrite, 8, 4, 0},
        {cosmos::SiteId::write, cosmos::FaultKind::ShortWrite, 5, 2, 0},
        {cosmos::SiteId::write, cosmos::FaultKind::ShortWrite, 1, 1, 0},
        {cosmos::SiteId::fsync, cosmos::FaultKind::FsyncEio, 0, -1, EIO},
        {cosmos::SiteId::fsync, cosmos::FaultKind::NoSpace, 0, -1, ENOSPC},
    };

    uint64_t seed = 0x51EED;
    for (const MatrixCase& c : cases) {
        const std::string path = temp_path("matrix");
        const int fd = open_scratch(path);
        const char source[] = "abcdefgh";
        if (c.site == cosmos::SiteId::read) {
            assert(::write(fd, source, sizeof(source) - 1) == 8);
        }
        assert(lseek(fd, 0, SEEK_SET) == 0);

        {
            cosmos::Simulator sim(seed++);
            must(sim.install_faults(storage_config(c.site, c.kind)).has_value());
            cosmos::Simulator::set_current(&sim);
            errno = 0;
            ssize_t result = 0;
            if (c.site == cosmos::SiteId::open) {
                result = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
            } else if (c.site == cosmos::SiteId::read) {
                char buf[8] = {};
                result = ::read(fd, buf, c.count);
            } else if (c.site == cosmos::SiteId::write) {
                result = ::write(fd, source, c.count);
            } else {
                result = fsync(fd);
            }
            const int error = errno;
            assert(result == c.result);
            assert(error == c.error);
            assert(c.error == 0 || listed_errno(c.site, error));
            assert(sim.injector_or_null()->eligible_calls(c.site) == 1);
            assert(sim.injector_or_null()->injections(c.site) == 1);
            cosmos::Simulator::set_current(nullptr);
        }

        if (c.site == cosmos::SiteId::write && c.result > 0) {
            assert(lseek(fd, 0, SEEK_SET) == 0);
            char back[8] = {};
            assert(::read(fd, back, sizeof(back)) == c.result);
            assert(memcmp(back, source, static_cast<size_t>(c.result)) == 0);
            assert(::read(fd, back, sizeof(back)) == 0);
        }

        close(fd);
        unlink(path.c_str());
    }
    std::cout << "[PASS] test_storage_outcome_matrix" << std::endl;
}

// A storage class that is off must not reach the injector at all, not merely decline to fire.
void test_disabled_storage_class_stays_passthrough() {
    const std::string path = temp_path("disabled");
    int fd = open_scratch(path);

    cosmos::Simulator sim(3);
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Memory);
    must(sim.install_faults(std::move(cfg)).has_value());
    cosmos::Simulator::set_current(&sim);

    const char msg[] = "intact";
    assert(::write(fd, msg, sizeof(msg) - 1) == 6);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    char buf[8] = {};
    assert(::read(fd, buf, sizeof(buf)) == 6);
    assert(memcmp(buf, msg, 6) == 0);
    assert(fsync(fd) == 0);
    const int second = ::open(path.c_str(), O_RDWR);
    assert(second >= 3);
    assert(::close(second) == 0);

    for (cosmos::SiteId site : {cosmos::SiteId::open, cosmos::SiteId::read, cosmos::SiteId::write,
                                cosmos::SiteId::fsync}) {
        assert(sim.injector_or_null()->eligible_calls(site) == 0);
        assert(sim.injector_or_null()->injections(site) == 0);
    }

    cosmos::Simulator::set_current(nullptr);
    close(fd);
    unlink(path.c_str());
    std::cout << "[PASS] test_disabled_storage_class_stays_passthrough" << std::endl;
}

int main() {
    // Unique 0700 directory: no dependence on a pre-existing path, no cross-user collisions.
    std::string tmpl = "/tmp/cosmos_storage_XXXXXX";
    if (mkdtemp(tmpl.data()) == nullptr) {
        std::cerr << "mkdtemp failed: " << strerror(errno) << std::endl;
        return 1;
    }
    g_temp_dir = tmpl;

    test_passthrough_no_sim();
    test_value_path_under_sim();
    test_zero_length_io_passthrough();
    test_std_stream_fds_pass_through();
    test_open_mode_forwarding();
    test_no_alloc_smoke();
    test_storage_outcomes_map_to_errno();
    test_fortified_read_alias_reaches_the_same_site();
    test_fortified_open_aliases_reach_the_same_site();
    test_faulted_calls_leave_no_side_effects();
    test_ineligible_fds_never_move_the_storage_stream();
    test_unactivated_site_never_moves_the_storage_stream();
    test_storage_outcome_matrix();
    test_disabled_storage_class_stays_passthrough();
    test_allocation_inside_wrapper_logic_is_never_faulted();

    rmdir(g_temp_dir.c_str()); // best effort; every test unlinks its own files
    std::cout << "All storage wrapper tests passed successfully!" << std::endl;
    return 0;
}
