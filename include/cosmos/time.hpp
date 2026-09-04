#pragma once

#include <cerrno>
#include <compare>
#include <concepts>
#include <cstdint>
#include <sys/time.h>
#include <time.h>

// glibc exposes TIMER_ABSTIME only under feature-test macros; define the POSIX value so the
// header stays self-contained under strict -std builds.
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

namespace cosmos {

// Signed overflow is UB, and a wrapped timestamp would silently reorder events in a run that is
// supposed to be reproducible. These saturate instead, so arithmetic is total and gives the same
// answer in debug and release. Same names and semantics as C++26 std::add_sat, which replaces
// them once the project moves off C++23.
constexpr int64_t add_sat(int64_t a, int64_t b) {
    int64_t result = 0;
    if (!__builtin_add_overflow(a, b, &result)) return result;
    return b > 0 ? INT64_MAX : INT64_MIN;
}

constexpr int64_t sub_sat(int64_t a, int64_t b) {
    int64_t result = 0;
    if (!__builtin_sub_overflow(a, b, &result)) return result;
    return b > 0 ? INT64_MIN : INT64_MAX;
}

constexpr int64_t mul_sat(int64_t a, int64_t b) {
    int64_t result = 0;
    if (!__builtin_mul_overflow(a, b, &result)) return result;
    return (a > 0) == (b > 0) ? INT64_MAX : INT64_MIN;
}

// Time and Duration are both a 64-bit nanosecond count, but they follow different rules: adding
// two timestamps is meaningless, adding two durations is not. The tag parameter keeps them
// separate types at compile time and costs nothing at runtime.
template <typename Tag> struct Nanos {
    int64_t ns{0};

    static constexpr Nanos zero() { return Nanos{0}; }
    static constexpr Nanos max() { return Nanos{INT64_MAX}; }
    static constexpr Nanos min() { return Nanos{INT64_MIN}; }

    constexpr bool operator==(const Nanos&) const = default;
    constexpr auto operator<=>(const Nanos&) const = default;
};

struct TimeTag {};
struct DurationTag {};

// Virtual time since universe start. Time::max() reads as "never" (the default quiesce_after),
// and saturating arithmetic keeps it that way.
using Time = Nanos<TimeTag>;
using Duration = Nanos<DurationTag>;

// Excludes floating point and bool: `d * 0.5` would convert to 0 and silently zero the duration,
// and `d * (x > 0)` is never what anyone meant. Use scale() for fractional factors.
template <typename K>
concept ScaleFactor = std::integral<K> && !std::same_as<K, bool>;

template <ScaleFactor K> constexpr int64_t to_factor(K k) {
    if constexpr (std::unsigned_integral<K>) {
        return static_cast<uintmax_t>(k) > static_cast<uintmax_t>(INT64_MAX)
                   ? INT64_MAX
                   : static_cast<int64_t>(k);
    } else {
        return static_cast<int64_t>(k);
    }
}

constexpr Duration operator+(Duration a, Duration b) { return Duration{add_sat(a.ns, b.ns)}; }
constexpr Duration operator-(Duration a, Duration b) { return Duration{sub_sat(a.ns, b.ns)}; }
constexpr Duration operator-(Duration d) { return Duration{sub_sat(0, d.ns)}; }

template <ScaleFactor K> constexpr Duration operator*(Duration d, K k) {
    return Duration{mul_sat(d.ns, to_factor(k))};
}
template <ScaleFactor K> constexpr Duration operator*(K k, Duration d) {
    return Duration{mul_sat(d.ns, to_factor(k))};
}

// Fractional scaling is a named call rather than an operator so the truncation is deliberate and
// visible at the call site. NaN yields zero; anything past the int64 range saturates.
constexpr Duration scale(Duration d, double factor) {
    const double result = static_cast<double>(d.ns) * factor;
    if (result != result) return Duration::zero();
    if (result <= static_cast<double>(INT64_MIN)) return Duration::min();
    if (result >= static_cast<double>(INT64_MAX)) return Duration::max();
    return Duration{static_cast<int64_t>(result)};
}

constexpr Time operator+(Time t, Duration d) { return Time{add_sat(t.ns, d.ns)}; }
constexpr Time operator+(Duration d, Time t) { return Time{add_sat(t.ns, d.ns)}; }
constexpr Time operator-(Time t, Duration d) { return Time{sub_sat(t.ns, d.ns)}; }
constexpr Duration operator-(Time a, Time b) { return Duration{sub_sat(a.ns, b.ns)}; }

constexpr Duration& operator+=(Duration& a, Duration b) { return a = a + b; }
constexpr Duration& operator-=(Duration& a, Duration b) { return a = a - b; }
template <ScaleFactor K> constexpr Duration& operator*=(Duration& d, K k) { return d = d * k; }
constexpr Time& operator+=(Time& t, Duration d) { return t = t + d; }
constexpr Time& operator-=(Time& t, Duration d) { return t = t - d; }

namespace literals {

constexpr int64_t to_ns(unsigned long long value, int64_t unit) {
    constexpr auto limit = static_cast<unsigned long long>(INT64_MAX);
    return mul_sat(value > limit ? INT64_MAX : static_cast<int64_t>(value), unit);
}

constexpr Duration operator""_ns(unsigned long long v) { return Duration{to_ns(v, 1)}; }
constexpr Duration operator""_us(unsigned long long v) { return Duration{to_ns(v, 1'000)}; }
constexpr Duration operator""_ms(unsigned long long v) { return Duration{to_ns(v, 1'000'000)}; }
constexpr Duration operator""_s(unsigned long long v) { return Duration{to_ns(v, 1'000'000'000)}; }

} // namespace literals

// Default realtime epoch: 2026-01-01 00:00:00 UTC = 1767225600 seconds = 1767225600000000000 ns
inline constexpr int64_t kDefaultRealtimeEpochNs = 1767225600000000000LL;

/**
 * @brief Deterministic virtual clock owned by a Simulator universe.
 *
 * Tracks monotonic virtual time (starting at 0ns) and wall-clock realtime (anchored at a
 * deterministic base epoch). In simulation testing builds, clock_gettime, gettimeofday, and
 * the sleep calls read and advance this clock without real-time OS delays.
 *
 * Interim contract: until the P1 fiber scheduler exists, sleeps advance the universe clock
 * by the full request immediately — nothing suspends. Once the scheduler lands
 * (docs/design.md §3), sleep calls must instead suspend the calling task and register a
 * wake-up event: virtual time only advances when no task is runnable, and one task's sleep
 * must not consume time for others.
 */
class VirtualClock {
  public:
    explicit VirtualClock(Time start_time = Time::zero(),
                          int64_t realtime_epoch_ns = kDefaultRealtimeEpochNs)
        : now_(start_time), realtime_epoch_ns_(realtime_epoch_ns) {}

    constexpr Time now() const { return now_; }
    constexpr int64_t now_ns() const { return now_.ns; }

    constexpr Time realtime() const { return Time{add_sat(realtime_epoch_ns_, now_.ns)}; }
    constexpr int64_t realtime_ns() const { return add_sat(realtime_epoch_ns_, now_.ns); }

    constexpr int64_t realtime_epoch_ns() const { return realtime_epoch_ns_; }
    void set_realtime_epoch(int64_t epoch_ns) { realtime_epoch_ns_ = epoch_ns; }

    void advance(Duration d) {
        if (d.ns <= 0) return;
        now_ = now_ + d;
    }

    void advance_to(Time target) {
        if (target > now_) {
            now_ = target;
        }
    }

    // Test-only repositioning: injector gate tests need to move the clock backward to
    // re-enter warmup/quiesce windows. Simulation code must use advance()/advance_to().
    void set(Time t) { now_ = t; }

    int clock_gettime(clockid_t clk_id, struct timespec* tp) const {
        if (!tp) {
            errno = EFAULT;
            return -1;
        }

        switch (clock_domain(clk_id)) {
        case ClockDomain::Monotonic:
            write_timespec(tp, now_ns());
            return 0;
        case ClockDomain::Realtime:
            write_timespec(tp, realtime_ns());
            return 0;
        case ClockDomain::Unsupported:
            break;
        }

        errno = EINVAL;
        return -1;
    }

    int gettimeofday(struct timeval* tv, void* tz) const {
        (void)tz;
        if (!tv) {
            errno = EFAULT;
            return -1;
        }
        int64_t ns = realtime_ns();
        int64_t us = ns / 1'000LL;
        tv->tv_sec = static_cast<time_t>(us / 1'000'000LL);
        tv->tv_usec = static_cast<suseconds_t>(us % 1'000'000LL);
        if (tv->tv_usec < 0) {
            tv->tv_sec -= 1;
            tv->tv_usec += 1'000'000LL;
        }
        return 0;
    }

    // Interim semantics: advances the universe clock by the full request. See the class
    // comment for the scheduler-era contract.
    int nanosleep(const struct timespec* req, struct timespec* rem) {
        if (!req) {
            errno = EFAULT;
            return -1;
        }
        if (!timespec_valid(*req)) {
            errno = EINVAL;
            return -1;
        }

        advance(Duration{timespec_ns(*req)});

        if (rem) {
            rem->tv_sec = 0;
            rem->tv_nsec = 0;
        }
        return 0;
    }

    // Unlike nanosleep, POSIX clock_nanosleep returns the error number directly instead of
    // -1 with errno. Production interruptions (EINTR plus a remainder) do not exist in the
    // simulation, so a valid sleep always completes in full and reports success.
    int clock_nanosleep(clockid_t clk_id, int flags, const struct timespec* req,
                        struct timespec* rem) {
        if (!req) {
            return EFAULT;
        }
        if (!timespec_valid(*req)) {
            return EINVAL;
        }
        if (flags != 0 && flags != TIMER_ABSTIME) {
            return EINVAL;
        }

        const ClockDomain domain = clock_domain(clk_id);
        if (domain == ClockDomain::Unsupported) {
            return EINVAL;
        }

        if (flags == TIMER_ABSTIME) {
            // Sleep until the requested clock reads req: a monotonic deadline is a raw
            // virtual reading, a realtime deadline is mapped through the epoch anchor.
            // A deadline in the past is a no-op.
            const int64_t deadline_ns = timespec_ns(*req);
            const Time target = domain == ClockDomain::Realtime
                                    ? Time{sub_sat(deadline_ns, realtime_epoch_ns_)}
                                    : Time{deadline_ns};
            advance_to(target);
        } else {
            advance(Duration{timespec_ns(*req)});
        }

        if (rem) {
            rem->tv_sec = 0;
            rem->tv_nsec = 0;
        }
        return 0;
    }

  private:
    enum class ClockDomain { Monotonic, Realtime, Unsupported };

    // Classifies the POSIX clock ids the simulation supports. Unsupported ones
    // (CLOCK_PROCESS_CPUTIME_ID and friends) fail with EINVAL rather than silently
    // reading a fake timeline.
    static constexpr ClockDomain clock_domain(clockid_t clk_id) {
        switch (clk_id) {
        case CLOCK_MONOTONIC:
#ifdef CLOCK_MONOTONIC_RAW
        case CLOCK_MONOTONIC_RAW:
#endif
#ifdef CLOCK_BOOTTIME
        case CLOCK_BOOTTIME:
#endif
#ifdef CLOCK_MONOTONIC_COARSE
        case CLOCK_MONOTONIC_COARSE:
#endif
            return ClockDomain::Monotonic;

        case CLOCK_REALTIME:
#ifdef CLOCK_REALTIME_COARSE
        case CLOCK_REALTIME_COARSE:
#endif
            return ClockDomain::Realtime;

        default:
            return ClockDomain::Unsupported;
        }
    }

    static constexpr bool timespec_valid(const struct timespec& ts) {
        return ts.tv_sec >= 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1'000'000'000L;
    }

    static constexpr int64_t timespec_ns(const struct timespec& ts) {
        return add_sat(mul_sat(static_cast<int64_t>(ts.tv_sec), 1'000'000'000LL),
                       static_cast<int64_t>(ts.tv_nsec));
    }

    static void write_timespec(struct timespec* tp, int64_t ns) {
        tp->tv_sec = static_cast<time_t>(ns / 1'000'000'000LL);
        tp->tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
        if (tp->tv_nsec < 0) {
            tp->tv_sec -= 1;
            tp->tv_nsec += 1'000'000'000LL;
        }
    }

    Time now_{Time::zero()};
    int64_t realtime_epoch_ns_{kDefaultRealtimeEpochNs};
};

} // namespace cosmos
