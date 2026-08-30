#pragma once

#include <cassert>
#include <cstdint>

namespace cosmos {

/// Seeds xoshiro256**'s state; also the recommended fix for xoshiro's "must not seed all-zero"
/// pitfall.
class SplitMix64 {
  public:
    explicit SplitMix64(uint64_t seed) : state_(seed) {}

    uint64_t next() {
        state_ += 0x9e3779b97f4a7c15ULL;
        uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

  private:
    uint64_t state_;
};

class Rng {
  public:
    explicit Rng(uint64_t seed) {
        SplitMix64 seeder(seed);
        state_[0] = seeder.next();
        state_[1] = seeder.next();
        state_[2] = seeder.next();
        state_[3] = seeder.next();
    }

    uint64_t next() {
        const uint64_t result = rotate_left(state_[1] * 5, 7) * 9;
        const uint64_t t = state_[1] << 17;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotate_left(state_[3], 45);

        return result;
    }

    // Top 53 bits give every representable double in [0,1) equal probability.
    double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

    // Rejects the low 2^64 % span draws so the surviving range divides evenly: plain modulo is
    // biased when the span doesn't divide 2^64.
    uint64_t range(uint64_t lo, uint64_t hi) {
        assert(lo <= hi);
        uint64_t span = hi - lo + 1;
        if (span == 0) return next();

        uint64_t threshold = (~span + 1) % span;
        uint64_t draw;
        do {
            draw = next();
        } while (draw < threshold);
        return lo + draw % span;
    }

    bool coin(double p) { return uniform() < p; }

  private:
    static uint64_t rotate_left(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    uint64_t state_[4];
};

enum class StreamDomain : uint64_t {
    Schedule = 1,
    Fault = 2,
    Workload = 3,
    User = 4,
    Swarm = 5,
};

// Mixes the index before combining so adjacent parents/indices decorrelate; uses SplitMix64's
// first output rather than its bare finalizer, which maps 0 to 0 and would collapse seed 0.
inline uint64_t derive_seed(uint64_t parent_seed, uint64_t index) {
    return SplitMix64(parent_seed ^ SplitMix64(index).next()).next();
}

inline uint64_t universe_seed(uint64_t campaign_seed, uint64_t universe_index) {
    return derive_seed(campaign_seed, universe_index);
}

inline uint64_t stream_seed(uint64_t parent_seed, StreamDomain domain) {
    return derive_seed(parent_seed, static_cast<uint64_t>(domain));
}

} // namespace cosmos
