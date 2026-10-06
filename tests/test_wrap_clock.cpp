#include "cosmos/cosmos.hpp"

// White-box include: this test pins the internal clock eligibility policy, not just the public
// behavior. See the target_include_directories entry in tests/CMakeLists.txt.
#include "wrapper_fault.hpp"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <time.h>

using cosmos::Duration;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultKind;
using cosmos::FaultRule;
using cosmos::Simulator;
using cosmos::SiteId;
using namespace cosmos::literals;

// Every kind a clock wrapper can emit must stay legal for its site (Rule 15); a taxonomy change
// that breaks one of these is a compile error here, not a false positive in a campaign.
static_assert(cosmos::is_legal_outcome(SiteId::clock_gettime, FaultKind::ClockStep));
static_assert(cosmos::is_legal_outcome(SiteId::nanosleep, FaultKind::SleepInterrupted));
static_assert(!cosmos::is_legal_outcome(SiteId::clock_gettime, FaultKind::SleepInterrupted));
static_assert(!cosmos::is_legal_outcome(SiteId::nanosleep, FaultKind::ClockStep));

// Sleep eligibility policy, pinned at compile time: only a well-formed, non-empty request can be
// interrupted, and an ineligible call must not spend a Clock draw (Rules 3 and 15).
inline constexpr struct timespec kEmpty{0, 0};
inline constexpr struct timespec kOneSecond{1, 0};
inline constexpr struct timespec kTwoAndAHalfSeconds{2, 500'000'000};
inline constexpr struct timespec kBadNanos{0, 1'000'000'000L};
inline constexpr struct timespec kNegativeSeconds{-1, 0};
inline constexpr struct timespec kNegativeNanos{0, -1};

static_assert(cosmos::wrappers::clock_timespec_ns(kEmpty) == 0);
static_assert(cosmos::wrappers::clock_timespec_ns(kOneSecond) == 1'000'000'000LL);
static_assert(cosmos::wrappers::clock_timespec_ns(kTwoAndAHalfSeconds) == 2'500'000'000LL);
static_assert(cosmos::wrappers::clock_timespec_ns(kBadNanos) == -1);
static_assert(cosmos::wrappers::clock_timespec_ns(kNegativeSeconds) == -1);
static_assert(cosmos::wrappers::clock_timespec_ns(kNegativeNanos) == -1);
static_assert(!cosmos::wrappers::clock_sleep_eligible(nullptr));

namespace {

void must(bool ok) { assert(ok); }

// The current reading of one simulated clock, in nanoseconds.
int64_t reading(clockid_t clock_id) {
    struct timespec now{};
    must(clock_gettime(clock_id, &now) == 0);
    return cosmos::wrappers::clock_timespec_ns(now);
}

// A Clock-class config with one rule on one site. max_injections bounds how many calls a test is
// willing to fault; the tests that count steps leave it unbounded.
FaultConfig clock_config(SiteId site, FaultKind kind, double rate, Duration elapsed = Duration{},
                         uint64_t max_injections = UINT64_MAX) {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Clock);
    must(cfg.activate_site(site));
    FaultRule rule;
    rule.rate = rate;
    rule.amount = elapsed;
    rule.max_injections = max_injections;
    must(rule.outcomes.add(kind, 1.0));
    must(cfg.set_rule(site, rule));
    return cfg;
}

// A universe with a config installed and itself set as current for the test's lifetime.
struct ClockUniverse {
    explicit ClockUniverse(FaultConfig cfg, uint64_t seed = 1) : sim(seed) {
        must(sim.install_faults(std::move(cfg)).has_value());
        Simulator::set_current(&sim);
    }
    ~ClockUniverse() { Simulator::set_current(nullptr); }

    ClockUniverse(const ClockUniverse&) = delete;
    ClockUniverse& operator=(const ClockUniverse&) = delete;

    Simulator sim;
};

struct SleepingSibling {
    bool entered = false;
};

void* sleep_for_a_second(void* arg) {
    auto* sibling = static_cast<SleepingSibling*>(arg);
    sibling->entered = true;
    struct timespec rem{};
    must(nanosleep(&kOneSecond, &rem) == 0);
    return nullptr;
}

} // namespace

// Without a universe the wrappers must reach the host, and a host monotonic clock only moves on.
void test_passthrough_without_a_simulator() {
    assert(!Simulator::has_current());

    const int64_t first = reading(CLOCK_MONOTONIC);
    const int64_t second = reading(CLOCK_MONOTONIC);
    assert(first >= 0);
    assert(second >= first);

    std::cout << "[PASS] test_passthrough_without_a_simulator" << std::endl;
}

// A universe with no injector installed cannot fire anything: the decision folds to None and every
// clock call behaves exactly as before.
void test_no_injector_is_passthrough_under_sim() {
    Simulator sim;
    Simulator::set_current(&sim);

    assert(reading(CLOCK_MONOTONIC) == 0);
    assert(reading(CLOCK_REALTIME) == cosmos::kDefaultRealtimeEpochNs);

    struct timespec rem{1, 1};
    must(nanosleep(&kOneSecond, &rem) == 0);
    assert(sim.clock().now_ns() == 1'000'000'000LL);
    assert(rem.tv_sec == 0 && rem.tv_nsec == 0);

    Simulator::set_current(nullptr);
    std::cout << "[PASS] test_no_injector_is_passthrough_under_sim" << std::endl;
}

// [happy] An injected step reaches the app, never goes backward, and is applied exactly once.
void test_clock_step_is_visible_and_forward() {
    ClockUniverse universe(clock_config(SiteId::clock_gettime, FaultKind::ClockStep, 1.0, 250_ms,
                                        /*max_injections=*/1));

    const int64_t stepped = reading(CLOCK_MONOTONIC);
    assert(stepped == 250'000'000LL); // visible to the app
    assert(stepped >= 0);             // never below the pre-step reading

    const int64_t later = reading(CLOCK_MONOTONIC);
    assert(later == stepped); // budget spent: the clock holds still
    assert(universe.sim.clock().now_ns() == 250'000'000LL);
    assert(universe.sim.injector_or_null()->injections(SiteId::clock_gettime) == 1);

    std::cout << "[PASS] test_clock_step_is_visible_and_forward" << std::endl;
}

// [happy] The step moves the universe, not just the reading being returned: gettimeofday takes no
// decision at all, yet it must show the stepped timeline (a per-call offset could not do this).
void test_step_moves_the_whole_universe() {
    ClockUniverse universe(clock_config(SiteId::clock_gettime, FaultKind::ClockStep, 1.0, 250_ms));

    assert(reading(CLOCK_MONOTONIC) == 250'000'000LL);

    struct timeval tv{};
    must(gettimeofday(&tv, nullptr) == 0);
    assert(tv.tv_sec == static_cast<time_t>(cosmos::kDefaultRealtimeEpochNs / 1'000'000'000LL));
    assert(tv.tv_usec == 250'000);

    std::cout << "[PASS] test_step_moves_the_whole_universe" << std::endl;
}

// [sad] Monotonicity survives a mixed sequence: a step is permanent, so no later reading falls
// below an earlier one, and every jump is exactly one step.
void test_monotonic_never_decreases() {
    ClockUniverse universe(clock_config(SiteId::clock_gettime, FaultKind::ClockStep, 0.3, 1_ms));

    int64_t previous = 0;
    uint64_t fires = 0;
    for (int i = 0; i < 10'000; ++i) {
        const int64_t now = reading(CLOCK_MONOTONIC);
        assert(now >= previous);
        const int64_t jumped = now - previous;
        assert(jumped == 0 || jumped == 1'000'000LL);
        if (jumped != 0) ++fires;
        previous = now;
    }
    assert(fires > 0 && fires < 10'000);
    assert(universe.sim.injector_or_null()->injections(SiteId::clock_gettime) == fires);

    // One timeline advanced, so every clock reading it agrees.
#ifdef CLOCK_MONOTONIC_RAW
    assert(reading(CLOCK_MONOTONIC_RAW) == previous);
#endif
#ifdef CLOCK_BOOTTIME
    assert(reading(CLOCK_BOOTTIME) == previous);
#endif
    assert(reading(CLOCK_REALTIME) == cosmos::kDefaultRealtimeEpochNs + previous);

    std::cout << "[PASS] test_monotonic_never_decreases" << std::endl;
}

// [sad] A step read through CLOCK_REALTIME moves the monotonic clock too: one universe timeline,
// with realtime as that same timeline anchored at its epoch.
void test_realtime_and_monotonic_share_one_timeline() {
    ClockUniverse universe(clock_config(SiteId::clock_gettime, FaultKind::ClockStep, 1.0, 400_ms,
                                        /*max_injections=*/1));

    const int64_t realtime = reading(CLOCK_REALTIME);
    assert(realtime == cosmos::kDefaultRealtimeEpochNs + 400'000'000LL);
    assert(reading(CLOCK_MONOTONIC) == 400'000'000LL);
    assert(realtime - reading(CLOCK_MONOTONIC) == cosmos::kDefaultRealtimeEpochNs);

    std::cout << "[PASS] test_realtime_and_monotonic_share_one_timeline" << std::endl;
}

// [sad] Rule 3 through the wrapper: an ineligible read never reaches the injector, so it cannot
// move the Clock stream, while an eligible one draws exactly as the stream predicts.
void test_ineligible_reads_never_move_the_clock_stream() {
    constexpr uint64_t kSeed = 0x510C4E5;
    ClockUniverse universe(clock_config(SiteId::clock_gettime, FaultKind::ClockStep, 0.5, 1_ms),
                           kSeed);

    cosmos::Rng reference(cosmos::fault_class_seed(
        cosmos::stream_seed(kSeed, cosmos::StreamDomain::Fault), FaultClass::Clock));

    int64_t expected_now = 0;
    uint64_t fires = 0;
    for (int i = 0; i < 60; ++i) {
        const bool expect_fire = reference.uniform() < 0.5;
        const int64_t now = reading(CLOCK_MONOTONIC);
        if (expect_fire) {
            expected_now += 1'000'000LL;
            ++fires;
        }
        assert(now == expected_now);
    }
    assert(fires > 0 && fires < 60);

    // An unsupported clock id and a missing output are real API errors, not faults (Rule 15), so
    // they must leave the eligible count and the stream position alone.
    auto* injector = universe.sim.injector_or_null();
    const uint64_t eligible_before_errors = injector->eligible_calls(SiteId::clock_gettime);
    struct timespec ignored{};
    errno = 0;
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ignored) == -1);
    assert(errno == EINVAL);
    struct timespec* no_output = nullptr;
    errno = 0;
    assert(clock_gettime(CLOCK_MONOTONIC, no_output) == -1);
    assert(errno == EFAULT);
    assert(injector->eligible_calls(SiteId::clock_gettime) == eligible_before_errors);

    // Still in step: the next eligible read lands exactly where the reference stream says.
    if (reference.uniform() < 0.5) {
        expected_now += 1'000'000LL;
        ++fires;
    }
    assert(reading(CLOCK_MONOTONIC) == expected_now);
    assert(injector->eligible_calls(SiteId::clock_gettime) == 61);
    assert(injector->injections(SiteId::clock_gettime) == fires);
    assert(injector->eligible_calls(SiteId::nanosleep) == 0);
    assert(injector->injections(SiteId::nanosleep) == 0);

    std::cout << "[PASS] test_ineligible_reads_never_move_the_clock_stream" << std::endl;
}

// [sad] With the Clock class disabled the gate stops the call before the eligible counter, so the
// site looks untouched rather than firing zero times out of many.
void test_a_disabled_clock_class_stays_passthrough() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Memory);
    ClockUniverse universe(std::move(cfg));

    for (int i = 0; i < 100; ++i) {
        assert(reading(CLOCK_MONOTONIC) == 0);
    }
    struct timespec rem{1, 1};
    must(nanosleep(&kOneSecond, &rem) == 0);
    assert(universe.sim.clock().now_ns() == 1'000'000'000LL);

    auto* injector = universe.sim.injector_or_null();
    assert(injector->eligible_calls(SiteId::clock_gettime) == 0);
    assert(injector->eligible_calls(SiteId::nanosleep) == 0);

    std::cout << "[PASS] test_a_disabled_clock_class_stays_passthrough" << std::endl;
}

// [sad] Zero strength means "no magnitude": the fire is still counted and recorded, but the
// reading itself does not move.
void test_zero_strength_step_is_counted_but_moves_nothing() {
    ClockUniverse universe(clock_config(SiteId::clock_gettime, FaultKind::ClockStep, 1.0));

    assert(reading(CLOCK_MONOTONIC) == 0);
    assert(universe.sim.clock().now_ns() == 0);
    assert(universe.sim.injector_or_null()->injections(SiteId::clock_gettime) == 1);

    std::cout << "[PASS] test_zero_strength_step_is_counted_but_moves_nothing" << std::endl;
}

// [happy] An interrupted sleep returns EINTR with the time it did not sleep, and the virtual clock
// really moved by the elapsed part.
void test_interrupted_sleep_reports_the_remainder() {
    ClockUniverse universe(
        clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0, 400_ms));

    struct timespec rem{9, 9};
    errno = 0;
    assert(nanosleep(&kOneSecond, &rem) == -1);
    assert(errno == EINTR);
    assert(rem.tv_sec == 0 && rem.tv_nsec == 600'000'000L);
    assert(universe.sim.clock().now_ns() == 400'000'000LL);
    assert(universe.sim.injector_or_null()->injections(SiteId::nanosleep) == 1);

    std::cout << "[PASS] test_interrupted_sleep_reports_the_remainder" << std::endl;
}

// [happy] The remainder is load-bearing: re-sleeping it lands exactly on the original deadline,
// while restarting the whole request overshoots -- the difference the fault exists to expose.
void test_remainder_is_load_bearing() {
    {
        ClockUniverse universe(clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0,
                                            400_ms, /*max_injections=*/1));
        struct timespec rem{};
        assert(nanosleep(&kOneSecond, &rem) == -1);
        assert(nanosleep(&rem, nullptr) == 0);
        assert(universe.sim.clock().now_ns() ==
               1'000'000'000LL); // honoured the remainder: deadline met
    }
    {
        ClockUniverse universe(clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0,
                                            400_ms, /*max_injections=*/1));
        struct timespec rem{};
        assert(nanosleep(&kOneSecond, &rem) == -1);
        assert(nanosleep(&kOneSecond, nullptr) == 0);
        assert(universe.sim.clock().now_ns() ==
               1'400'000'000LL); // restarted the request: overshoot
    }

    std::cout << "[PASS] test_remainder_is_load_bearing" << std::endl;
}

// [sad] Interrupting on entry (zero elapsed) is legal but degenerate: nothing moves, and the
// remainder is the whole request.
void test_interrupt_without_elapsed_time() {
    ClockUniverse universe(clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0));

    struct timespec rem{9, 9};
    errno = 0;
    assert(nanosleep(&kOneSecond, &rem) == -1);
    assert(errno == EINTR);
    assert(rem.tv_sec == 1 && rem.tv_nsec == 0);
    assert(universe.sim.clock().now_ns() == 0);
    assert(universe.sim.injector_or_null()->injections(SiteId::nanosleep) == 1);

    std::cout << "[PASS] test_interrupt_without_elapsed_time" << std::endl;
}

// [sad] When the elapsed part covers the whole request there is no remainder left to report, so
// the sleep completes -- the degradation ShortSend takes on a one-byte write.
void test_elapsed_covering_the_request_degrades_to_a_complete_sleep() {
    ClockUniverse universe(clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0, 2_s));

    struct timespec half{0, 500'000'000};
    errno = 0;
    assert(nanosleep(&half, nullptr) == 0);
    assert(errno == 0);
    assert(universe.sim.clock().now_ns() == 500'000'000LL);
    assert(universe.sim.injector_or_null()->injections(SiteId::nanosleep) == 1);

    std::cout << "[PASS] test_elapsed_covering_the_request_degrades_to_a_complete_sleep"
              << std::endl;
}

// [sad] Requests the scheduler itself rejects cannot be interrupted, so they spend no draw.
void test_ineligible_sleeps_never_move_the_clock_stream() {
    ClockUniverse universe(
        clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0, 400_ms));

    const struct timespec* no_request = nullptr;
    errno = 0;
    assert(nanosleep(no_request, nullptr) == -1);
    assert(errno == EFAULT);
    errno = 0;
    assert(nanosleep(&kBadNanos, nullptr) == -1);
    assert(errno == EINVAL);
    errno = 0;
    assert(nanosleep(&kEmpty, nullptr) == 0);
    assert(errno == 0);

    // The same policy the scheduler applies, stated directly: only a well-formed, non-empty
    // request is interruptible.
    assert(!cosmos::wrappers::clock_sleep_eligible(no_request));
    assert(!cosmos::wrappers::clock_sleep_eligible(&kEmpty));
    assert(!cosmos::wrappers::clock_sleep_eligible(&kBadNanos));
    assert(!cosmos::wrappers::clock_sleep_eligible(&kNegativeSeconds));
    assert(!cosmos::wrappers::clock_sleep_eligible(&kNegativeNanos));
    assert(cosmos::wrappers::clock_sleep_eligible(&kOneSecond));

    auto* injector = universe.sim.injector_or_null();
    assert(injector->eligible_calls(SiteId::nanosleep) == 0);
    assert(injector->injections(SiteId::nanosleep) == 0);
    assert(universe.sim.clock().now_ns() == 0);

    std::cout << "[PASS] test_ineligible_sleeps_never_move_the_clock_stream" << std::endl;
}

// [happy] A relative clock_nanosleep is the same operation behind a different convention: it
// returns the error number itself and never touches errno.
void test_clock_nanosleep_relative_is_interrupted() {
    ClockUniverse universe(
        clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0, 300_ms));

    struct timespec rem{9, 9};
    errno = 0;
    assert(clock_nanosleep(CLOCK_MONOTONIC, 0, &kOneSecond, &rem) == EINTR);
    assert(errno == 0);
    assert(rem.tv_sec == 0 && rem.tv_nsec == 700'000'000L);
    assert(universe.sim.clock().now_ns() == 300'000'000LL);

    std::cout << "[PASS] test_clock_nanosleep_relative_is_interrupted" << std::endl;
}

// [sad] An absolute sleep is restarted with the same deadline, so its remainder must be left
// untouched -- clock_nanosleep(2) calls that argument unused for TIMER_ABSTIME.
void test_absolute_sleep_leaves_the_remainder_untouched() {
    ClockUniverse universe(
        clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0, 400_ms));

    struct timespec rem{7, 7};
    assert(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &kOneSecond, &rem) == EINTR);
    assert(rem.tv_sec == 7 && rem.tv_nsec == 7);
    assert(universe.sim.clock().now_ns() == 400'000'000LL);

    std::cout << "[PASS] test_absolute_sleep_leaves_the_remainder_untouched" << std::endl;
}

// [happy] An absolute request names a deadline, so what can be interrupted is the time left until
// it, not the deadline itself.
void test_absolute_sleep_counts_down_from_now() {
    ClockUniverse universe(
        clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0, 700_ms));

    // Reach 500ms, then ask for a 1s deadline: only 500ms of it remain, so the 700ms elapsed part
    // covers the whole request and the sleep simply completes. Reading the deadline as the interval
    // instead would interrupt at 700ms and overshoot the deadline entirely.
    struct timespec half{0, 500'000'000};
    assert(nanosleep(&half, nullptr) == 0);
    assert(universe.sim.clock().now_ns() == 500'000'000LL);
    struct timespec rem{7, 7};
    assert(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &kOneSecond, &rem) == 0);
    assert(universe.sim.clock().now_ns() == 1'000'000'000LL);
    assert(rem.tv_sec == 7 && rem.tv_nsec == 7); // unused for an absolute sleep, even on success

    std::cout << "[PASS] test_absolute_sleep_counts_down_from_now" << std::endl;
}

// [sad] A deadline already reached is a no-op with nothing to interrupt, so it spends no draw.
void test_past_deadline_is_never_eligible() {
    ClockUniverse universe(
        clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0, 400_ms));

    assert(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &kEmpty, nullptr) == 0);
    assert(universe.sim.clock().now_ns() == 0);
    assert(universe.sim.injector_or_null()->eligible_calls(SiteId::nanosleep) == 0);

    std::cout << "[PASS] test_past_deadline_is_never_eligible" << std::endl;
}

// [sad] Both sleep APIs share one site, so one rule and one budget cover them, the way __read_chk
// counts as a read.
void test_both_sleep_apis_share_one_site() {
    ClockUniverse universe(clock_config(SiteId::nanosleep, FaultKind::SleepInterrupted, 1.0,
                                        Duration{}, /*max_injections=*/1));

    struct timespec rem{};
    assert(clock_nanosleep(CLOCK_MONOTONIC, 0, &kOneSecond, &rem) == EINTR);
    auto* injector = universe.sim.injector_or_null();
    assert(injector->injections(SiteId::nanosleep) == 1);
    assert(universe.sim.clock().now_ns() == 0);

    // The budget is spent, so the plain nanosleep now runs to completion.
    assert(nanosleep(&kOneSecond, nullptr) == 0);
    assert(universe.sim.clock().now_ns() == 1'000'000'000LL);
    assert(injector->injections(SiteId::nanosleep) == 1);
    assert(injector->eligible_calls(SiteId::nanosleep) == 2);

    std::cout << "[PASS] test_both_sleep_apis_share_one_site" << std::endl;
}

// [sad] Rule 7 for the clock family: a call from inside wrapper logic is answered by the
// universe, takes no decision, and never leaks host time into a deterministic run.
void test_clock_calls_inside_wrapper_logic_are_virtual_and_unfaulted() {
    ClockUniverse universe(clock_config(SiteId::clock_gettime, FaultKind::ClockStep, 1.0, 250_ms));

    {
        cosmos::wrappers::ReentrancyGuard guard;

        assert(reading(CLOCK_MONOTONIC) == 0); // the virtual reading, not the host's uptime

        struct timespec rem{9, 9};
        must(nanosleep(&kOneSecond, &rem) == 0);
        assert(rem.tv_sec == 0 && rem.tv_nsec == 0);
        assert(universe.sim.clock().now_ns() == 1'000'000'000LL);

        assert(clock_nanosleep(CLOCK_MONOTONIC, 0, &kOneSecond, nullptr) == 0);
        assert(universe.sim.clock().now_ns() == 2'000'000'000LL);
    }

    auto* injector = universe.sim.injector_or_null();
    assert(injector->eligible_calls(SiteId::clock_gettime) == 0);
    assert(injector->eligible_calls(SiteId::nanosleep) == 0);
    assert(injector->injections(SiteId::clock_gettime) == 0);
    assert(injector->injections(SiteId::nanosleep) == 0);

    std::cout << "[PASS] test_clock_calls_inside_wrapper_logic_are_virtual_and_unfaulted"
              << std::endl;
}

// Rule 7 is about engine work, not about whoever happens to be sleeping: a fiber suspended in a
// wrapped sleep must not leave in_wrapper_logic set for its siblings sharing the OS thread, or
// their calls would silently skip the injector.
void test_sleep_does_not_leak_wrapper_logic_to_siblings() {
    ClockUniverse universe(clock_config(SiteId::clock_gettime, FaultKind::ClockStep, 1.0, 5_ms),
                           /*seed=*/7);

    SleepingSibling sibling;
    pthread_t sleeper = 0;
    must(pthread_create(&sleeper, nullptr, sleep_for_a_second, &sibling) == 0);

    // The sleeping fiber runs until its nanosleep suspends it, so once `entered` is visible the
    // sleep is in progress and control is back here.
    for (int i = 0; i < 16 && !sibling.entered; ++i) {
        sched_yield();
    }
    must(sibling.entered);
    assert(!cosmos::wrappers::in_wrapper_logic);

    // A wrapped call now is an application call and must take its decision. Under a guard held
    // across the sleep it would have been answered as engine work, spending no draw.
    assert(reading(CLOCK_MONOTONIC) == 5'000'000LL);
    auto* injector = universe.sim.injector_or_null();
    assert(injector->eligible_calls(SiteId::clock_gettime) == 1);
    assert(injector->injections(SiteId::clock_gettime) == 1);

    void* retval = nullptr;
    must(pthread_join(sleeper, &retval) == 0);
    assert(universe.sim.clock().now_ns() == 1'000'000'000LL);

    std::cout << "[PASS] test_sleep_does_not_leak_wrapper_logic_to_siblings" << std::endl;
}

int main() {
    test_passthrough_without_a_simulator();
    test_no_injector_is_passthrough_under_sim();
    test_clock_step_is_visible_and_forward();
    test_step_moves_the_whole_universe();
    test_monotonic_never_decreases();
    test_realtime_and_monotonic_share_one_timeline();
    test_ineligible_reads_never_move_the_clock_stream();
    test_a_disabled_clock_class_stays_passthrough();
    test_zero_strength_step_is_counted_but_moves_nothing();
    // The behavioural test owns the remainder property, so it runs first and names it in the
    // failure; the exact-value test follows.
    test_remainder_is_load_bearing();
    test_interrupted_sleep_reports_the_remainder();
    test_interrupt_without_elapsed_time();
    test_elapsed_covering_the_request_degrades_to_a_complete_sleep();
    test_ineligible_sleeps_never_move_the_clock_stream();
    test_clock_nanosleep_relative_is_interrupted();
    test_absolute_sleep_leaves_the_remainder_untouched();
    test_absolute_sleep_counts_down_from_now();
    test_past_deadline_is_never_eligible();
    test_both_sleep_apis_share_one_site();
    test_clock_calls_inside_wrapper_logic_are_virtual_and_unfaulted();
    test_sleep_does_not_leak_wrapper_logic_to_siblings();

    std::cout << "All clock wrapper tests passed successfully!" << std::endl;
    return 0;
}
