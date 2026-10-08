#pragma once

// Application-facing property checks (docs/design.md §12). They report into the thread's current
// Scenario, so an app never has to thread a harness object through its own code.

#include "cosmos/scenario.hpp"
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace cosmos {
namespace detail {

// Where an assertion goes when no universe is bound to the calling thread. Counted and echoed
// rather than asserted (or silently dropped): the lifecycle rule in scenario.hpp is that a release
// build must not pass by ignoring one, while a build with no simulation running must not be killed
// by one either. S1's campaign surfaces `count` in its report.
struct OutOfScopeAssertions {
    uint64_t count = 0;
    std::string first_id;
    std::string first_detail;
};

inline OutOfScopeAssertions& out_of_scope_assertions() {
    static OutOfScopeAssertions state;
    return state;
}

// One lock covers record-and-echo, so a parallel campaign cannot interleave two reports.
inline std::mutex& out_of_scope_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline void record_out_of_scope(const char* caller, const std::string& id,
                                const std::string& detail) {
    std::lock_guard<std::mutex> lock(out_of_scope_mutex());
    OutOfScopeAssertions& state = out_of_scope_assertions();
    if (state.count == 0) {
        state.first_id = id;
        state.first_detail = detail;
        std::fprintf(stderr, "[cosmos] %s(\"%s\") outside a Scenario: %s\n", caller, id.c_str(),
                     detail.c_str());
    }
    ++state.count;
}

} // namespace detail

// An invariant of this universe: a violation is recorded at once, a pass is silent.
// Valid inside Scenario::run(), Scenario::check() and the quiesce drain; use Scenario::check()
// for anything that must be judged after the world has settled.
inline void always(bool cond, std::string id, std::string detail = "") {
    if (cond) return;
    Scenario* scenario = Scenario::current();
    if (scenario == nullptr) {
        ::cosmos::detail::record_out_of_scope("always", id, detail);
        return;
    }
    scenario->record_violation(std::move(id), std::move(detail));
}

// COSMOS_CHECK's arm: the call site is materialized only once the check has already failed, so a
// passing check allocates nothing and cannot shift a universe's own accounting.
inline void always(bool cond, std::string id, const char* file, int line) {
    if (cond) return;
    always(false, std::move(id), std::string(file) + ":" + std::to_string(line));
}

// A liveness property that must hold in at least one universe: the ids no universe reports are the
// campaign's never_hit list. A universe that reported only hits registers no check, so it still
// reads as vacuous() -- sometimes() is a campaign-level fact, not this universe's verdict.
inline void sometimes(bool cond, std::string id) {
    Scenario* scenario = Scenario::current();
    if (scenario == nullptr) {
        ::cosmos::detail::record_out_of_scope("sometimes", id, cond ? "hit" : "not hit");
        return;
    }
    scenario->record_sometimes(std::move(id), cond);
}

// Marks a path as exercised, without naming a condition.
inline void reachable(std::string id) { sometimes(true, std::move(id)); }

} // namespace cosmos

// The call site is the detail, so an unnamed check still says where it failed.
#define COSMOS_CHECK(cond, id) ::cosmos::always((cond), (id), __FILE__, __LINE__)
