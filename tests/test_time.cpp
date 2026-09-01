#include "cosmos/cosmos.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <sys/time.h>
#include <time.h>

// Stage 1 exit criterion (docs/state-exploration.md Summary): clock_gettime in
// sim returns virtual nanoseconds, not wall time.

void test_clock_starts_at_zero() {
    cosmos::VirtualClock clock;
    assert(clock.now_ns() == 0);
    std::cout << "[PASS] test_clock_starts_at_zero" << std::endl;
}

void test_advance_is_forward_only() {
    cosmos::VirtualClock clock;

    clock.advance_to(1'000);
    assert(clock.now_ns() == 1'000);

    // Monotonicity invariant: a backward request leaves the clock untouched
    // (a CLOCK_MONOTONIC read may never step backward).
    clock.advance_to(500);
    assert(clock.now_ns() == 1'000);

    clock.advance_to(1'000);
    assert(clock.now_ns() == 1'000);

    std::cout << "[PASS] test_advance_is_forward_only" << std::endl;
}

void test_sleep_semantics() {
    cosmos::VirtualClock clock;

    assert(clock.sleep_for(250) == 250);
    assert(clock.now_ns() == 250);

    assert(clock.sleep_until(1'000) == 1'000);
    assert(clock.now_ns() == 1'000);

    // A sleep into the past is a no-op, never a backward jump.
    assert(clock.sleep_for(-500) == 1'000);
    assert(clock.sleep_until(0) == 1'000);

    std::cout << "[PASS] test_sleep_semantics" << std::endl;
}

void test_set_is_for_snapshot_restore() {
    cosmos::VirtualClock clock;
    clock.advance_to(5'000);
    clock.set(100);
    assert(clock.now_ns() == 100);
    std::cout << "[PASS] test_set_is_for_snapshot_restore" << std::endl;
}

void test_clock_is_deterministic() {
    // The clock is a pure function of execution history: two universes that
    // issue the same advance sequence always read the same time.
    const int64_t deltas[] = {100, 50, 500, 1'000, 250};
    int64_t a = 0;
    int64_t b = 0;

    cosmos::VirtualClock clock_a;
    for (int64_t d : deltas) {
        a = clock_a.sleep_for(d);
    }

    cosmos::VirtualClock clock_b;
    for (int64_t d : deltas) {
        b = clock_b.sleep_for(d);
    }

    assert(a == b);
    assert(a == 1'900);
    std::cout << "[PASS] test_clock_is_deterministic" << std::endl;
}

void test_wrapped_clock_gettime_without_sim() {
    // Outside an active simulation the wrapper falls through to the real host
    // clock (epoch time, so tv_sec must be decades past zero).
    struct timespec ts;
    int rc = clock_gettime(CLOCK_REALTIME, &ts);
    assert(rc == 0);
    assert(ts.tv_sec > 1'000'000);
    std::cout << "[PASS] test_wrapped_clock_gettime_without_sim" << std::endl;
}

void test_wrapped_clock_gettime_returns_virtual_time() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0);
    assert(ts.tv_sec == 0 && ts.tv_nsec == 0);

    assert(sim.clock().sleep_until(1'500'000'000) == 1'500'000'000);
    rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0);
    assert(ts.tv_sec == 1 && ts.tv_nsec == 500'000'000);

    // Every clock ID reads the same single simulated clock.
    struct timespec rt;
    rc = clock_gettime(CLOCK_REALTIME, &rt);
    assert(rc == 0);
    assert(rt.tv_sec == ts.tv_sec && rt.tv_nsec == ts.tv_nsec);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_wrapped_clock_gettime_returns_virtual_time" << std::endl;
}

void test_wrapped_gettimeofday_returns_virtual_time() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    assert(sim.clock().sleep_for(2'000'250'000) == 2'000'250'000);

    struct timeval tv;
    int rc = gettimeofday(&tv, nullptr);
    assert(rc == 0);
    assert(tv.tv_sec == 2);
    assert(tv.tv_usec == 250);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_wrapped_gettimeofday_returns_virtual_time" << std::endl;
}

void test_wrapped_nanosleep_advances_virtual_time() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    struct timespec req = {.tv_sec = 0, .tv_nsec = 750'000'000};
    struct timespec rem = {.tv_sec = 99, .tv_nsec = 99};

    int rc = nanosleep(&req, &rem);
    assert(rc == 0);
    assert(sim.clock().now_ns() == 750'000'000);

    // A completed virtual sleep is never interrupted: rem stays zero and EINTR
    // does not exist in a deterministic universe.
    assert(rem.tv_sec == 0 && rem.tv_nsec == 0);

    // Successive sleeps accumulate; sleeping into the past is a no-op.
    req = {.tv_sec = 1, .tv_nsec = 0};
    rc = nanosleep(&req, &rem);
    assert(rc == 0);
    assert(sim.clock().now_ns() == 1'750'000'000);

    req = {.tv_sec = 0, .tv_nsec = 0};
    rc = nanosleep(&req, &rem);
    assert(rc == 0);
    assert(sim.clock().now_ns() == 1'750'000'000);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_wrapped_nanosleep_advances_virtual_time" << std::endl;
}

void test_two_sims_read_identical_time() {
    // Same sleep sequence through two independent simulators => identical
    // observations through the wrapped POSIX surface.
    struct timespec req = {.tv_sec = 0, .tv_nsec = 400'000'000};
    struct timespec a, b;

    cosmos::Simulator sim_a;
    cosmos::Simulator::set_current(&sim_a);
    nanosleep(&req, nullptr);
    nanosleep(&req, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &a);
    cosmos::Simulator::set_current(nullptr);

    cosmos::Simulator sim_b;
    cosmos::Simulator::set_current(&sim_b);
    nanosleep(&req, nullptr);
    nanosleep(&req, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &b);
    cosmos::Simulator::set_current(nullptr);

    assert(a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec);
    std::cout << "[PASS] test_two_sims_read_identical_time" << std::endl;
}

int main() {
    test_clock_starts_at_zero();
    test_advance_is_forward_only();
    test_sleep_semantics();
    test_set_is_for_snapshot_restore();
    test_clock_is_deterministic();
    test_wrapped_clock_gettime_without_sim();
    test_wrapped_clock_gettime_returns_virtual_time();
    test_wrapped_gettimeofday_returns_virtual_time();
    test_wrapped_nanosleep_advances_virtual_time();
    test_two_sims_read_identical_time();
    std::cout << "All time tests passed successfully!" << std::endl;
    return 0;
}
