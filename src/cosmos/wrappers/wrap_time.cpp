// POSIX time wrappers route to the current universe's VirtualClock; without an active
// Simulator on this thread they fall through to the real host clock. Sleeps go
// through the fiber scheduler so one task's sleep suspends only itself and
// virtual time advances only when no fiber is runnable (docs/design.md §10).
//
// Clock faults (docs/fault-injection.md §8.2): a step moves the whole universe's clock, and an
// interrupted sleep reports how much of the request is left.
#include "cosmos/cosmos.hpp"

#include "wrapper_fault.hpp"

#include <cerrno>
#include <cstdint>
#include <sys/time.h>
#include <time.h>

extern "C" {

int __real_clock_gettime(clockid_t clock_id, struct timespec* tp);
int __real_gettimeofday(struct timeval* tv, void* tz);
int __real_nanosleep(const struct timespec* req, struct timespec* rem);
int __real_clock_nanosleep(clockid_t clock_id, int flags, const struct timespec* req,
                           struct timespec* rem);

namespace {

// Sleeps the calling fiber for exactly `ns` of virtual time, leaving the caller's remainder alone.
void sleep_for(cosmos::Simulator& sim, int64_t ns) {
    const struct timespec request{static_cast<time_t>(ns / 1'000'000'000LL),
                                  static_cast<long>(ns % 1'000'000'000LL)};
    sim.scheduler().sleep_ns(&request, nullptr);
}

// Writes how much of the original sleep is left, the way nanosleep(2) reports it after EINTR.
void write_remainder(struct timespec* rem, int64_t ns) {
    if (rem == nullptr) return;
    rem->tv_sec = static_cast<time_t>(ns / 1'000'000'000LL);
    rem->tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
}

} // namespace

int __wrap_clock_gettime(clockid_t clock_id, struct timespec* tp) {
    if (!cosmos::Simulator::has_current()) {
        return __real_clock_gettime(clock_id, tp);
    }
    // Rule 7: a call arriving from inside wrapper logic is answered by the universe but takes no
    // decision. Passing through to the host instead would leak a real clock into a deterministic
    // run, which is the opposite of what the guard is for.
    const bool engine_call = cosmos::wrappers::in_wrapper_logic;
    cosmos::wrappers::ReentrancyGuard guard;

    cosmos::Simulator* sim = cosmos::Simulator::current();
    if (engine_call) {
        return sim->clock().clock_gettime(clock_id, tp);
    }
    // Reading the clock changes nothing, so a failed read is a real API error rather than a fault:
    // it never reaches the injector and never spends a Clock draw (Rules 3 and 15).
    if (const int rc = sim->clock().clock_gettime(clock_id, tp); rc != 0) {
        return rc;
    }

    const cosmos::wrappers::Decision fault = cosmos::wrappers::decide_with_amount(
        sim, cosmos::FaultClass::Clock, cosmos::SiteId::clock_gettime);
    if (fault.kind == cosmos::FaultKind::ClockStep) {
        // Forward and permanent: validate() rejects a negative amount, and moving the universe
        // clock is what keeps every later reading monotonic (Rule 15).
        sim->clock().advance(fault.amount);
        return sim->clock().clock_gettime(clock_id, tp);
    }
    return 0;
}

int __wrap_nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!cosmos::Simulator::has_current()) {
        return __real_nanosleep(req, rem);
    }
    // Sampled before the guard: creating the guard sets the flag.
    const bool engine_call = cosmos::wrappers::in_wrapper_logic;
    cosmos::Simulator* sim = cosmos::Simulator::current();
    // A missing, malformed, or zero-length request has nothing to interrupt, so it is answered by
    // the scheduler's own validation and never spends a Clock draw (Rules 3 and 15).
    if (engine_call || !cosmos::wrappers::clock_sleep_eligible(req)) {
        return sim->scheduler().sleep_ns(req, rem);
    }
    const int64_t requested_ns = cosmos::wrappers::clock_timespec_ns(*req);

    // Scoped to the decision only: the sleeps below suspend this fiber and run siblings
    // on the same OS thread, which must not observe in_wrapper_logic (fibers share it).
    cosmos::wrappers::Decision fault;
    {
        cosmos::wrappers::ReentrancyGuard guard;
        fault = cosmos::wrappers::decide_with_amount(sim, cosmos::FaultClass::Clock,
                                                     cosmos::SiteId::nanosleep);
    }
    if (fault.kind != cosmos::FaultKind::SleepInterrupted || fault.amount.ns >= requested_ns) {
        // Nothing to interrupt, or the elapsed part covers the whole request: sleep normally. The
        // fire is still counted, just as ShortSend falls back to a full write on one byte.
        return sim->scheduler().sleep_ns(req, rem);
    }
    // Really sleep the elapsed part, so an interrupted sleep still moves virtual time forward and
    // leaves the caller a non-trivial remainder to sleep again.
    if (fault.amount.ns > 0) {
        sleep_for(*sim, fault.amount.ns);
    }
    write_remainder(rem, requested_ns - fault.amount.ns);
    errno = EINTR;
    return -1;
}

int __wrap_clock_nanosleep(clockid_t clock_id, int flags, const struct timespec* req,
                           struct timespec* rem) {
    if (!cosmos::Simulator::has_current()) {
        return __real_clock_nanosleep(clock_id, flags, req, rem);
    }
    const bool engine_call = cosmos::wrappers::in_wrapper_logic;
    cosmos::Simulator* sim = cosmos::Simulator::current();
    // Same site as nanosleep: the same operation, differing only in how the caller names the time
    // and where the error goes. The wrapper owns that difference (docs/design.md §8.2).
    if (engine_call || !cosmos::wrappers::clock_sleep_eligible(req) ||
        (flags != 0 && flags != TIMER_ABSTIME)) {
        return sim->scheduler().sleep_clock(clock_id, flags, req, rem);
    }
    int64_t sleep_ns = cosmos::wrappers::clock_timespec_ns(*req);

    if (flags == TIMER_ABSTIME) {
        // An absolute request names a deadline, not a length, so only the time left until it can be
        // interrupted. Both readings come from the caller's clock, so no epoch maths is needed.
        // Invalid clocks are real API errors, never draws (validates before decide, like
        // clock_gettime).
        struct timespec clock_now{};
        if (sim->clock().clock_gettime(clock_id, &clock_now) != 0) {
            return sim->scheduler().sleep_clock(clock_id, flags, req, rem); // real error, no draw
        }
        sleep_ns -= cosmos::wrappers::clock_timespec_ns(clock_now);
        if (sleep_ns <= 0) {
            return sim->scheduler().sleep_clock(clock_id, flags, req, rem); // deadline already past
        }
    } else {
        // Relative sleeps name no clock reading, but a bad clock_id is still a real API
        // error: validate before spending a Clock draw, mirroring the absolute path.
        struct timespec probe{};
        if (sim->clock().clock_gettime(clock_id, &probe) != 0) {
            return sim->scheduler().sleep_clock(clock_id, flags, req, rem); // real error, no draw
        }
    }

    // Scoped to the decision only (see nanosleep): sleeps below yield the fiber.
    cosmos::wrappers::Decision fault;
    {
        cosmos::wrappers::ReentrancyGuard guard;
        fault = cosmos::wrappers::decide_with_amount(sim, cosmos::FaultClass::Clock,
                                                     cosmos::SiteId::nanosleep);
    }
    if (fault.kind != cosmos::FaultKind::SleepInterrupted || fault.amount.ns >= sleep_ns) {
        return sim->scheduler().sleep_clock(clock_id, flags, req, rem);
    }
    if (fault.amount.ns > 0) {
        sleep_for(*sim, fault.amount.ns);
    }
    // An absolute sleep is restarted with the same deadline instead, so its remainder stays
    // untouched (clock_nanosleep(2): "the remain argument is unused ... when flags is
    // TIMER_ABSTIME").
    if (flags == 0) {
        write_remainder(rem, sleep_ns - fault.amount.ns);
    }
    return EINTR; // returned directly, not -1 plus errno
}

int __wrap_gettimeofday(struct timeval* tv, void* tz) {
    if (!cosmos::Simulator::has_current()) {
        return __real_gettimeofday(tv, tz);
    }
    return cosmos::Simulator::current()->clock().gettimeofday(tv, tz);
}

} // extern "C"
