#include "cosmos/cosmos.hpp"
#include <cerrno>
#include <sys/time.h>
#include <time.h>

// Linker-wrapping implementation of the POSIX time surface over the
// deterministic VirtualClock (docs/design.md §3 "Time", docs/state-exploration.md
// Summary Stage 1).
//
// With an active Simulator, every clock read returns virtual nanoseconds —
// never wall time — and nanosleep advances virtual time instead of blocking
// the host thread. Exit criterion (Stage 1): `clock_gettime` in sim returns
// virtual nanoseconds, not wall time.
//
// Outside an active simulation the wrappers fall through to the real host
// functions, keeping libcosmos usable in unmanaged contexts exactly like the
// other __wrap_* surfaces (see wrap_memory.cpp).

extern "C" {

int __real_clock_gettime(clockid_t clock_id, struct timespec* tp);
int __real_gettimeofday(struct timeval* tv, void* tz);
int __real_nanosleep(const struct timespec* req, struct timespec* rem);

/**
 * @brief Linker interposition wrapper for POSIX `clock_gettime(clk_id, tp)`.
 *
 * WHAT IT IS:
 * Intercepts all calls to `clock_gettime` at final link time when compiled with
 * `-Wl,--wrap=clock_gettime`.
 *
 * WHY & WHEN IT IS USED:
 * - Used during testing builds (`-DCOSMOS_SIM`) so the application observes the
 *   deterministic VirtualClock instead of the host kernel clock. The virtual
 *   clock starts at zero and only moves when the simulation moves it, so the
 *   same execution history always yields the same timestamps.
 * - All clock IDs (`CLOCK_REALTIME`, `CLOCK_MONOTONIC`, ...) report the same
 *   virtual time: in a simulated universe there is exactly one clock.
 * - Outside an active simulation, falls back to `__real_clock_gettime`.
 *
 * @return 0 on success. Never fails in sim mode; no clock exists that can be
 * unavailable.
 */
int __wrap_clock_gettime(clockid_t clock_id, struct timespec* tp) {
    if (!cosmos::Simulator::has_current()) {
        return __real_clock_gettime(clock_id, tp);
    }

    const int64_t ns = cosmos::Simulator::current()->clock().now_ns();
    tp->tv_sec = static_cast<time_t>(ns / 1'000'000'000);
    tp->tv_nsec = static_cast<long>(ns % 1'000'000'000);
    return 0;
}

/**
 * @brief Linker interposition wrapper for POSIX `gettimeofday(tv, tz)`.
 *
 * WHY & WHEN IT IS USED:
 * - Same contract as `__wrap_clock_gettime`: with an active Simulator the
 *   reported wall time is the VirtualClock, so code using gettimeofday and
 *   code using clock_gettime agree on the simulated "now".
 * - The tz argument is a legacy hole that glibc always sets to NULL; it stays
 *   untouched, matching real-kernel behavior.
 * - Outside an active simulation, falls back to `__real_gettimeofday`.
 *
 * @return 0 on success.
 */
int __wrap_gettimeofday(struct timeval* tv, void* tz) {
    if (!cosmos::Simulator::has_current()) {
        return __real_gettimeofday(tv, tz);
    }

    const int64_t ns = cosmos::Simulator::current()->clock().now_ns();
    tv->tv_sec = static_cast<time_t>(ns / 1'000'000'000);
    tv->tv_usec = static_cast<suseconds_t>((ns % 1'000'000'000) / 1'000);
    return 0;
}

/**
 * @brief Linker interposition wrapper for POSIX `nanosleep(req, rem)`.
 *
 * WHY & WHEN IT IS USED:
 * - Used during testing builds (`-DCOSMOS_SIM`) so a sleeping task advances
 *   deterministic virtual time rather than stalling the host thread.
 * - Stage 1: the sleep advances the VirtualClock directly to
 *   `now + req` and returns immediately — there is no scheduler yet to
 *   suspend a fiber and let other tasks run. The returned time is clamped to
 *   be monotonic, matching VirtualClock::sleep_until.
 * - `*rem` is always zeroed: a completed virtual sleep is never interrupted,
 *   and EINTR does not exist in a deterministic universe.
 * - Stage 2 will replace the direct advance with task suspension plus
 *   event-driven clock advance on quiescence (docs/design.md §5).
 * - Outside an active simulation, falls back to `__real_nanosleep`.
 *
 * @return 0 on success (virtual sleep always completes).
 */
int __wrap_nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!cosmos::Simulator::has_current()) {
        return __real_nanosleep(req, rem);
    }

    const int64_t req_ns = static_cast<int64_t>(req->tv_sec) * 1'000'000'000 +
                           static_cast<int64_t>(req->tv_nsec);
    auto* sim = cosmos::Simulator::current();
    const int64_t now = sim->clock().now_ns();
    // Clamp instead of overflowing: signed overflow is UB, and INT64_MAX reads as "never".
    const int64_t target =
        req_ns > INT64_MAX - now ? INT64_MAX : now + req_ns;
    (void)sim->clock().sleep_until(target);

    if (rem != nullptr) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

} // extern "C"
