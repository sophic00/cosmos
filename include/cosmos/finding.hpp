#pragma once

#include <cstdint>
#include <string>

namespace cosmos {

// One recorded violation: an always() breach, a crashing universe, or a determinism-contract
// violation caught by --verify (state-exploration.md §4). ids and details are std::string_view
// at the call site so the happy path allocates nothing; a Failure materializes std::strings
// only when a violation is actually recorded.
struct Failure {
    uint64_t seed;
    std::string assertion_id; // "kv.read-your-writes", "crash", "determinism.violation", ...
    std::string detail;       // file:line from COSMOS_ALWAYS, or the diverging hash pair
};

} // namespace cosmos
