#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cosmos {

// FNV-1a (64-bit) over a canonical little-endian event stream. This is the interim encoding of
// trace_hash() (state-exploration.md §4): the digest grows incrementally as events occur, never
// reconstructed from a stored log, so the original run and a verify re-run each hash their own
// live stream and the two digests are compared at the end. When fault-injection.md §11.3 pins
// the full event encodings (scheduler, network, storage), they absorb through the same
// absorb_* calls without changing the digest interface.
//
// Field tags are mixed in before each value so a stream of N values can never collide with a
// different (tag, value) alignment of the same bytes.
class TraceHash {
  public:
    void absorb_tag(uint8_t tag) {
        state_ ^= tag;
        state_ *= kPrime;
    }

    void absorb_u64(uint64_t value) {
        uint8_t bytes[8];
        // Canonical little-endian: identical on every target, big- or little-endian host.
        for (int i = 0; i < 8; ++i) {
            bytes[i] = static_cast<uint8_t>(value >> (i * 8));
        }
        absorb_bytes(bytes, sizeof(bytes));
    }

    void absorb_i64(int64_t value) { absorb_u64(static_cast<uint64_t>(value)); }

    void absorb_bool(bool value) { absorb_tag(value ? 1 : 0); }

    uint64_t digest() const { return state_; }

  private:
    void absorb_bytes(const uint8_t* bytes, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            state_ ^= bytes[i];
            state_ *= kPrime;
        }
    }

    static constexpr uint64_t kPrime = 0x100000001b3ULL;
    static constexpr uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
    uint64_t state_{kOffsetBasis};
};

// Event tags shared by every absorb site. Append-only: existing values must never change
// meaning, or old trace hashes (and repro reports) silently shift.
enum class TraceEvent : uint8_t {
    UniverseSeed = 1,
    ClockAdvance = 2,   // relative advance (advance/nanosleep family)
    ClockAdvanceTo = 3, // absolute reposition (advance_to/clock_nanosleep ABSTIME)
    FaultFire = 4,
};

// Canonical fold of the stream digests that live in different owners (the Simulator absorbs
// seed + clock events; the injector's ledger absorbs fault fires). Both inputs are maintained
// incrementally; this only combines the two digests, it never replays a stored log. Append a
// TraceEvent tag here before folding more components in.
inline uint64_t combine_trace_hashes(uint64_t a, uint64_t b) {
    TraceHash fold;
    fold.absorb_tag(0xFF); // tag: digest-combine node
    fold.absorb_u64(a);
    fold.absorb_u64(b);
    return fold.digest();
}

} // namespace cosmos
