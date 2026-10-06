#include "cosmos/cosmos.hpp"
#include "cosmos/scenario.hpp"
#include "cosmos/task.hpp"
#include "wrapper_fault.hpp"

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <time.h>
#include <vector>

namespace {

void must(bool ok) { assert(ok); }
void must_eq(int actual, int expected) { assert(actual == expected); }

struct SimGuard {
    cosmos::Simulator sim;
    explicit SimGuard(uint64_t seed) : sim(seed) { cosmos::Simulator::set_current(&sim); }
    ~SimGuard() { cosmos::Simulator::set_current(nullptr); }
};

// ---- 1. spawn / join / retval ----

void* ret42(void*) { return reinterpret_cast<void*>(static_cast<uintptr_t>(42)); }

void test_spawn_join_basic() {
    SimGuard guard(1234);
    pthread_t thread = 0;
    must_eq(pthread_create(&thread, nullptr, ret42, nullptr), 0);
    must(thread != 0);
    void* retval = nullptr;
    must_eq(pthread_join(thread, &retval), 0);
    must(reinterpret_cast<uintptr_t>(retval) == 42);
    std::cout << "[PASS] test_spawn_join_basic" << std::endl;
}

void test_join_bad_id_is_esrch() {
    SimGuard guard(1234);
    must_eq(pthread_join(static_cast<pthread_t>(999999), nullptr), ESRCH);
    // Joining main (id 0) is not a spawned fiber.
    must_eq(pthread_join(static_cast<pthread_t>(0), nullptr), ESRCH);
    std::cout << "[PASS] test_join_bad_id_is_esrch" << std::endl;
}

void* self_probe(void* out) {
    auto* id = static_cast<pthread_t*>(out);
    *id = pthread_self();
    return nullptr;
}

void test_self_and_equal() {
    SimGuard guard(1234);
    pthread_t main_self = pthread_self();
    must(main_self == 0);
    must(pthread_equal(main_self, main_self) != 0);
    pthread_t worker_self = 0;
    pthread_t thread = 0;
    must_eq(pthread_create(&thread, nullptr, self_probe, &worker_self), 0);
    must_eq(pthread_join(thread, nullptr), 0);
    must(worker_self == thread);
    must(worker_self != main_self);
    must(pthread_equal(worker_self, thread) != 0);
    must(pthread_equal(worker_self, main_self) == 0);
    std::cout << "[PASS] test_self_and_equal" << std::endl;
}

void* noop_fn(void*) { return nullptr; }

void test_detach_then_join_is_einval() {
    SimGuard guard(1234);
    pthread_t thread = 0;
    must_eq(pthread_create(&thread, nullptr, noop_fn, nullptr), 0);
    must_eq(pthread_detach(thread), 0);
    must_eq(pthread_join(thread, nullptr), EINVAL);
    // Second detach is also an error.
    must_eq(pthread_detach(thread), EINVAL);
    must_eq(pthread_detach(static_cast<pthread_t>(777777)), ESRCH);
    // Let the detached fiber finish via the drain.
    guard.sim.scheduler().run_until_quiescence();
    must(!guard.sim.scheduler().deadlocked());
    std::cout << "[PASS] test_detach_then_join_is_einval" << std::endl;
}

// ---- 2. mutex-protected counter ----

namespace counter_case {

constexpr int kThreads = 4;
constexpr int kIters = 1000;
int counter = 0;
pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

void* worker(void*) {
    for (int i = 0; i < kIters; ++i) {
        must_eq(pthread_mutex_lock(&mu), 0);
        ++counter;
        must_eq(pthread_mutex_unlock(&mu), 0);
    }
    return nullptr;
}

} // namespace counter_case

void test_mutex_protects_counter() {
    SimGuard guard(777);
    using namespace counter_case;
    counter = 0;
    must_eq(pthread_mutex_init(&mu, nullptr), 0);
    pthread_t threads[kThreads];
    for (int i = 0; i < kThreads; ++i)
        must_eq(pthread_create(&threads[i], nullptr, worker, nullptr), 0);
    for (int i = 0; i < kThreads; ++i)
        must_eq(pthread_join(threads[i], nullptr), 0);
    must(counter == kThreads * kIters);
    must_eq(pthread_mutex_destroy(&mu), 0);
    std::cout << "[PASS] test_mutex_protects_counter" << std::endl;
}

void test_mutex_self_relock_is_edeadlk() {
    SimGuard guard(777);
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    must_eq(pthread_mutex_lock(&mu), 0);
    must_eq(pthread_mutex_lock(&mu), EDEADLK);
    must_eq(pthread_mutex_unlock(&mu), 0);
    must_eq(pthread_mutex_unlock(&mu), EPERM);
    std::cout << "[PASS] test_mutex_self_relock_is_edeadlk" << std::endl;
}

void test_mutex_trylock() {
    SimGuard guard(777);
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    must_eq(pthread_mutex_trylock(&mu), 0);
    must_eq(pthread_mutex_trylock(&mu), EBUSY);
    must_eq(pthread_mutex_unlock(&mu), 0);
    must_eq(pthread_mutex_trylock(&mu), 0);
    must_eq(pthread_mutex_unlock(&mu), 0);
    std::cout << "[PASS] test_mutex_trylock" << std::endl;
}

// ---- 3. deterministic interleaving ----

namespace interleave_case {

constexpr int kIters = 6;
int trace[2 * kIters];
int trace_len = 0;

void* worker_a(void*) {
    for (int i = 0; i < kIters; ++i) {
        trace[trace_len++] = 1;
        sched_yield();
    }
    return nullptr;
}

void* worker_b(void*) {
    for (int i = 0; i < kIters; ++i) {
        trace[trace_len++] = 2;
        sched_yield();
    }
    return nullptr;
}

std::string run_once(uint64_t seed) {
    cosmos::Simulator sim(seed);
    cosmos::Simulator::set_current(&sim);
    trace_len = 0;
    pthread_t ta = 0;
    pthread_t tb = 0;
    must_eq(pthread_create(&ta, nullptr, worker_a, nullptr), 0);
    must_eq(pthread_create(&tb, nullptr, worker_b, nullptr), 0);
    must_eq(pthread_join(ta, nullptr), 0);
    must_eq(pthread_join(tb, nullptr), 0);
    cosmos::Simulator::set_current(nullptr);
    std::string out;
    for (int i = 0; i < trace_len; ++i)
        out += static_cast<char>('0' + trace[i]);
    return out;
}

} // namespace interleave_case

void test_same_seed_same_interleaving() {
    using namespace interleave_case;
    const std::string first = run_once(999);
    must(static_cast<int>(first.size()) == 2 * kIters);
    must(first == run_once(999));
    must(first != run_once(1000) || run_once(1001) != run_once(1002));
    // Across a sweep both orders appear: the schedule stream is actually used.
    bool saw_start_1 = false;
    bool saw_start_2 = false;
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        const std::string trace = run_once(seed);
        must(static_cast<int>(trace.size()) == 2 * kIters);
        if (trace[0] == '1') saw_start_1 = true;
        if (trace[0] == '2') saw_start_2 = true;
    }
    must(saw_start_1 && saw_start_2);
    std::cout << "[PASS] test_same_seed_same_interleaving first=" << first << std::endl;
}

// ---- 4. cond producer/consumer (no lost wakeup) ----

namespace cond_case {

pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
bool started = false;
bool ready = false;
int consumed = 0;

void* consumer(void*) {
    must_eq(pthread_mutex_lock(&mu), 0);
    started = true;
    while (!ready)
        must_eq(pthread_cond_wait(&cond, &mu), 0);
    ++consumed;
    must_eq(pthread_mutex_unlock(&mu), 0);
    return nullptr;
}

} // namespace cond_case

void test_cond_wait_signal() {
    SimGuard guard(4242);
    using namespace cond_case;
    started = false;
    ready = false;
    consumed = 0;
    must_eq(pthread_mutex_init(&mu, nullptr), 0);
    must_eq(pthread_cond_init(&cond, nullptr), 0);
    pthread_t thread = 0;
    must_eq(pthread_create(&thread, nullptr, consumer, nullptr), 0);
    // Wait until the consumer is actually parked, so the signal cannot be lost
    // regardless of which interleaving the seed picks.
    while (!started)
        sched_yield();
    // Give the consumer one more chance to reach cond_wait (started is set
    // before the wait, so a second yield covers the gap deterministically
    // without relying on schedule order).
    sched_yield();
    sched_yield();
    must_eq(pthread_mutex_lock(&mu), 0);
    ready = true;
    must_eq(pthread_cond_signal(&cond), 0);
    must_eq(pthread_mutex_unlock(&mu), 0);
    must_eq(pthread_join(thread, nullptr), 0);
    must(consumed == 1);
    must_eq(pthread_mutex_destroy(&mu), 0);
    must_eq(pthread_cond_destroy(&cond), 0);
    std::cout << "[PASS] test_cond_wait_signal" << std::endl;
}

namespace bcast_case {

constexpr int kWaiters = 3;
pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
int ready_flag = 0;
int woken = 0;
int arrived = 0;

void* waiter(void*) {
    must_eq(pthread_mutex_lock(&mu), 0);
    ++arrived;
    while (ready_flag == 0)
        must_eq(pthread_cond_wait(&cond, &mu), 0);
    ++woken;
    must_eq(pthread_mutex_unlock(&mu), 0);
    return nullptr;
}

} // namespace bcast_case

void test_cond_broadcast_wakes_all() {
    SimGuard guard(5150);
    using namespace bcast_case;
    ready_flag = 0;
    woken = 0;
    arrived = 0;
    pthread_t threads[kWaiters];
    for (int i = 0; i < kWaiters; ++i)
        must_eq(pthread_create(&threads[i], nullptr, waiter, nullptr), 0);
    while (arrived < kWaiters)
        sched_yield();
    for (int i = 0; i < 2 * kWaiters + 2; ++i)
        sched_yield();
    must_eq(pthread_mutex_lock(&mu), 0);
    ready_flag = 1;
    must_eq(pthread_cond_broadcast(&cond), 0);
    must_eq(pthread_mutex_unlock(&mu), 0);
    for (int i = 0; i < kWaiters; ++i)
        must_eq(pthread_join(threads[i], nullptr), 0);
    must(woken == kWaiters);
    std::cout << "[PASS] test_cond_broadcast_wakes_all" << std::endl;
}

// ---- 5. sleeps order by time, block only self ----

namespace sleep_case {

int order[2];
int order_len = 0;
pthread_mutex_t order_mu = PTHREAD_MUTEX_INITIALIZER;

void* sleeper_10ms(void*) {
    struct timespec req{0, 10 * 1000 * 1000};
    must(nanosleep(&req, nullptr) == 0);
    must_eq(pthread_mutex_lock(&order_mu), 0);
    order[order_len++] = 10;
    must_eq(pthread_mutex_unlock(&order_mu), 0);
    return nullptr;
}

void* sleeper_5ms(void*) {
    struct timespec req{0, 5 * 1000 * 1000};
    must(nanosleep(&req, nullptr) == 0);
    must_eq(pthread_mutex_lock(&order_mu), 0);
    order[order_len++] = 5;
    must_eq(pthread_mutex_unlock(&order_mu), 0);
    return nullptr;
}

} // namespace sleep_case

void test_sleep_orders_by_time() {
    SimGuard guard(31337);
    using namespace sleep_case;
    order_len = 0;
    pthread_t ta = 0;
    pthread_t tb = 0;
    must_eq(pthread_create(&ta, nullptr, sleeper_10ms, nullptr), 0);
    must_eq(pthread_create(&tb, nullptr, sleeper_5ms, nullptr), 0);
    must_eq(pthread_join(ta, nullptr), 0);
    must_eq(pthread_join(tb, nullptr), 0);
    must(order_len == 2);
    must(order[0] == 5 && order[1] == 10);
    must(guard.sim.now() >= cosmos::Time::zero() + cosmos::Duration{10 * 1000 * 1000});
    std::cout << "[PASS] test_sleep_orders_by_time" << std::endl;
}

namespace sleep_self_case {

bool flag = false;

void* sleeper(void*) {
    struct timespec req{0, 5 * 1000 * 1000};
    must(nanosleep(&req, nullptr) == 0);
    flag = true;
    return nullptr;
}

} // namespace sleep_self_case

void test_sleep_blocks_only_self() {
    SimGuard guard(7777);
    using namespace sleep_self_case;
    flag = false;
    pthread_t thread = 0;
    must_eq(pthread_create(&thread, nullptr, sleeper, nullptr), 0);
    // The sleeper has not run yet: spawning never preempts.
    must(guard.sim.now() == cosmos::Time::zero());
    int work = 0;
    for (int i = 0; i < 100; ++i)
        ++work;
    must(work == 100);
    must(guard.sim.now() == cosmos::Time::zero());
    must_eq(pthread_join(thread, nullptr), 0);
    must(flag);
    must(guard.sim.now() >= cosmos::Time::zero() + cosmos::Duration{5 * 1000 * 1000});
    std::cout << "[PASS] test_sleep_blocks_only_self" << std::endl;
}

void test_abs_sleep_in_past_is_noop() {
    SimGuard guard(7777);
    struct timespec past{0, 0};
    must(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &past, nullptr) == 0);
    must(guard.sim.now() == cosmos::Time::zero());
    std::cout << "[PASS] test_abs_sleep_in_past_is_noop" << std::endl;
}

// ---- 6. deadlock is reported, not hung ----

namespace deadlock_case {

pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

void* waiter(void*) {
    must_eq(pthread_mutex_lock(&mu), 0);
    // Nobody will ever signal: the drain must report, not hang.
    must_eq(pthread_cond_wait(&cond, &mu), 0);
    must_eq(pthread_mutex_unlock(&mu), 0);
    return nullptr;
}

} // namespace deadlock_case

void test_deadlock_reported_by_drain() {
    cosmos::Simulator sim(60606);
    cosmos::Simulator::set_current(&sim);
    pthread_t thread = 0;
    must_eq(pthread_create(&thread, nullptr, deadlock_case::waiter, nullptr), 0);
    // Do not join: return to the drain with the waiter still parked.
    cosmos::Simulator::set_current(nullptr);
    cosmos::Simulator::set_current(&sim);
    sim.scheduler().run_until_quiescence();
    must(sim.scheduler().deadlocked());
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_deadlock_reported_by_drain" << std::endl;
}

void test_scenario_quiesce_reports_deadlock() {
    auto scenario = cosmos::Scenario::create(60606, cosmos::FaultPlan{});
    must(scenario.has_value());
    scenario->run([] {
        pthread_t thread = 0;
        must_eq(pthread_create(&thread, nullptr, deadlock_case::waiter, nullptr), 0);
    });
    scenario->quiesce();
    must(!scenario->passed());
    bool found = false;
    for (const auto& check : scenario->report().checks) {
        if (check.id == std::string(cosmos::kLifecycleId) &&
            check.detail.find("deadlock") != std::string::npos) {
            found = true;
        }
    }
    must(found);
    std::cout << "[PASS] test_scenario_quiesce_reports_deadlock" << std::endl;
}

// ---- 7. engine work never enters the scheduler ----

// Direct calls, not pthread_self(): glibc declares pthread_self __attribute__((__const__)),
// so the compiler is free to fold several calls into one and reuse the first result — fatal
// here, where consecutive calls must observe different routing decisions.
extern "C" pthread_t __wrap_pthread_self(void);

void test_engine_internal_calls_passthrough() {
    // Regression: std::mutex::lock() is header inline, so the allocation registry's mutex
    // reaches __wrap_pthread_mutex_lock from inside __wrap_malloc itself (in_wrapper_logic
    // set) whenever the pthread family is wrapped. Engine sync must use the real OS
    // primitives: routing it into the scheduler would yield a fiber mid-wrapper —
    // mutex_unlock always reschedules — while every other fiber still sees
    // in_wrapper_logic and silently bypasses allocation tracking. pthread_self() is the
    // discriminator: the main fiber's id is 0, a real OS thread's is never 0.
    cosmos::Simulator sim(9101);
    cosmos::Simulator::set_current(&sim);
    must(__wrap_pthread_self() == 0);
    pthread_mutex_t host_mu = PTHREAD_MUTEX_INITIALIZER;
    {
        cosmos::wrappers::ReentrancyGuard engine_context;
        must(pthread_mutex_lock(&host_mu) == 0);
        must(pthread_mutex_unlock(&host_mu) == 0);
        must(__wrap_pthread_self() != 0);
    }
    must(__wrap_pthread_self() == 0);
    // The scheduler never learned about the engine's mutex: an app-level lock of the same
    // object starts from a clean sim-mutex state.
    must(pthread_mutex_lock(&host_mu) == 0);
    must(pthread_mutex_unlock(&host_mu) == 0);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_engine_internal_calls_passthrough" << std::endl;
}

} // namespace

int main() {
    test_spawn_join_basic();
    test_join_bad_id_is_esrch();
    test_self_and_equal();
    test_detach_then_join_is_einval();
    test_mutex_protects_counter();
    test_mutex_self_relock_is_edeadlk();
    test_mutex_trylock();
    test_same_seed_same_interleaving();
    test_cond_wait_signal();
    test_cond_broadcast_wakes_all();
    test_sleep_orders_by_time();
    test_sleep_blocks_only_self();
    test_abs_sleep_in_past_is_noop();
    test_deadlock_reported_by_drain();
    test_scenario_quiesce_reports_deadlock();
    test_engine_internal_calls_passthrough();
    std::cout << "All scheduler tests passed successfully!" << std::endl;
    return 0;
}
