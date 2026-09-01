#pragma once

#include <cstdint>

namespace cosmos {

/// Deterministic virtual clock: linear nanoseconds since universe start
/// (docs/design.md §1 "Time", docs/state-exploration.md Summary Stage 1).
///
/// Virtual time does NOT advance during CPU computation. It moves forward
/// only via explicit jumps — sleeps and (once the Stage-2 fiber scheduler
/// exists) event dispatch to the next scheduled event timestamp. This makes
/// the guest clock a pure function of deterministic state and execution
/// history (docs/antithesis-study-notes.md).
///
/// Monotonic by construction: a CLOCK_MONOTONIC read may only step forward,
/// never backward (docs/fault-injection.md legality rules — no real kernel
/// produces a backward monotonic clock, so a simulation must not either).
///
/// The clock is never touched by ambient entropy: no seed, no wall-time
/// reads, no floating point. Two universes that issue the same sequence of
/// advances always read the same time.
class VirtualClock {
  public:
    /// Nanoseconds since universe start. Starts at zero.
    int64_t now_ns() const { return now_ns_; }

    /// Advances the clock to `ns`. Forward-only: a backward request leaves
    /// the clock untouched (monotonicity invariant).
    void advance_to(int64_t ns) {
        if (ns > now_ns_) {
            now_ns_ = ns;
        }
    }

    /// Suspends for `d` nanoseconds, returning the virtual time actually
    /// reached. Stage 1 advances the clock directly; the fiber scheduler
    /// (Stage 2) replaces this with task suspension plus event-driven
    /// advance to the next scheduled timestamp on quiescence.
    int64_t sleep_for(int64_t d) { return sleep_until(now_ns_ + d); }

    /// Suspends until `ns`, returning the virtual time actually reached.
    int64_t sleep_until(int64_t ns) {
        advance_to(ns);
        return now_ns_;
    }

    /// Absolute set for snapshot restore (docs/state-exploration.md §6:
    /// `clock_.set(snap.virtual_time_ns)`). Not for use during a normal run:
    /// unlike advance_to, it can move the clock backward.
    void set(int64_t ns) { now_ns_ = ns; }

  private:
    int64_t now_ns_{0};
};

} // namespace cosmos
