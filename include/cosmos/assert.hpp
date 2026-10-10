#pragma once

#include "cosmos/finding.hpp"
#include "cosmos/simulator.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cosmos {

// Invariant: checked every time it is called within a universe (state-exploration.md §4). A
// single violation = one Failure recorded into the current universe's findings. The happy path
// allocates nothing: id and detail are views, and std::strings materialize only on violation.
//
// detail defaults to empty; COSMOS_ALWAYS captures file:line at compile time instead.
inline void always(bool cond, std::string_view id, std::string_view detail = {}) {
    if (cond) {
        return;
    }
    if (!Simulator::has_current()) {
        // A violation with no universe to record it into is a harness bug: the finding would be
        // silently lost. Loudly refuse rather than drop it.
        std::fputs("cosmos::always violation outside any Simulator context (id: ", stderr);
        std::fwrite(id.data(), 1, id.size(), stderr);
        std::fputs(")\n", stderr);
        std::abort();
    }
    Simulator& sim = *Simulator::current();
    sim.record_finding(Failure{
        .seed = sim.seed(),
        .assertion_id = std::string(id),
        .detail = std::string(detail),
    });
}

#define COSMOS_STR2(x) #x
#define COSMOS_STR(x) COSMOS_STR2(x)

// Preferred call form: file:line is captured at compile time — zero runtime cost, no per-call
// formatting. This is what harness code should use.
#define COSMOS_ALWAYS(cond, id) ::cosmos::always((cond), (id), __FILE__ ":" COSMOS_STR(__LINE__))

// Campaign-wide liveness registry behind sometimes() (state-exploration.md §4). Thread-safe:
// worker threads register ids and mark hits concurrently; a single mutex is acceptable because
// these calls are rare relative to simulation work.
//
// The id is REGISTERED on every call and marked hit only when cond is true. Registration on
// every call is what makes never_hit work: if the assertion is never satisfied anywhere, the
// campaign still knows the id exists and reports it. A version that records only on success
// can never discover an unsatisfied id.
class SometimesRegistry {
  public:
    void record(std::string_view id, bool hit) {
        const std::lock_guard<std::mutex> lock(mutex_);
        Entry& entry = entries_[std::string(id)];
        ++entry.registrations;
        if (hit) {
            ++entry.hits;
        }
    }

    void reset() {
        const std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    // Ids registered but never satisfied anywhere, sorted for stable output.
    std::vector<std::string> never_hit_ids() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> never_hit;
        for (const auto& [id, entry] : entries_) {
            if (entry.hits == 0) {
                never_hit.push_back(id);
            }
        }
        return never_hit;
    }

    // Every registered id, sorted for stable output.
    std::vector<std::string> registered_ids() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> ids;
        ids.reserve(entries_.size());
        for (const auto& [id, entry] : entries_) {
            ids.push_back(id);
        }
        return ids;
    }

    uint64_t registrations(std::string_view id) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(std::string(id));
        return found == entries_.end() ? 0 : found->second.registrations;
    }

    uint64_t hits(std::string_view id) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(std::string(id));
        return found == entries_.end() ? 0 : found->second.hits;
    }

  private:
    struct Entry {
        uint64_t registrations = 0;
        uint64_t hits = 0;
    };

    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_; // std::map: sorted iteration without a second sort
};

// Campaign-wide singleton. When Campaign::run() lands (campaign.hpp), Campaign::current()
// delegates to this.
inline SometimesRegistry& sometimes_registry() {
    static SometimesRegistry registry;
    return registry;
}

// Liveness: evaluated campaign-wide. Must be satisfied (cond == true) in at least ONE universe.
// See SometimesRegistry for why registration happens on every call.
inline void sometimes(bool cond, std::string_view id) { sometimes_registry().record(id, cond); }

} // namespace cosmos
