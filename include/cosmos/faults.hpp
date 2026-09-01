#pragma once

#include "cosmos/random.hpp"
#include <cstdint>

namespace cosmos {

enum class FaultClass : uint8_t { Memory, Network, Storage, Clock, Process, _Count };

// Each class draws from its own sub-stream so changing one class's config cannot shift another's
// draws (docs/fault-injection.md §7 Rule 2).
inline uint64_t fault_class_seed(uint64_t fault_stream_seed, FaultClass fault_class) {
    return derive_seed(fault_stream_seed, static_cast<uint64_t>(fault_class));
}

struct FaultProfile {
    double oom_rate = 0.0; // Heap allocation failure probability [0.0, 1.0]

    // Decides one allocation attempt. Endpoint rates never draw (docs/fault-injection.md §7
    // Rule 3): a disabled or always-on fault must not shift the stream, so intermediate rates
    // are the only case that consumes a decision.
    bool should_inject_oom(Rng& rng) const {
        if (oom_rate <= 0.0) return false;
        if (oom_rate >= 1.0) return true;
        return rng.uniform() < oom_rate;
    }
};

} // namespace cosmos
