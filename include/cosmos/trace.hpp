#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/time.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cosmos {

// One entry per post-gate decision, in the order the decisions happened (§11.2). Gate-blocked
// calls never reach the boundary and are counted instead, so the ledger stays fires-only.
struct TraceEntry {
    uint64_t seq;
    uint64_t eligible_index;
    Time at;
    SiteId site;
    Duration amount;
    FaultKind outcome;
    bool drew;
    DecisionStatus status = DecisionStatus::Fired;

    constexpr bool operator==(const TraceEntry&) const = default;
};

// 1M entries is ~48 MB, allocated once per universe, and only when a trace was asked for.
inline constexpr uint64_t kDefaultTraceCapacity = 1 << 20;

struct TraceOptions {
    bool enabled = false;
    uint64_t capacity = kDefaultTraceCapacity;
};

// Fixed capacity, never grows: decide() runs inside __wrap_malloc, so recording must not allocate
// (Rule 7). Overflow is counted rather than dropped silently, like FaultLedger's.
class FaultTrace {
  public:
    FaultTrace() = default;

    explicit FaultTrace(uint64_t capacity) : capacity_(capacity) {
        if (capacity_ > 0) entries_.reserve(static_cast<size_t>(capacity_));
    }

    bool enabled() const { return capacity_ > 0; }
    uint64_t capacity() const { return capacity_; }
    uint64_t dropped() const { return dropped_; }

    // Which universe wrote this, so a replay needs no seed argument. Zero until a Simulator names
    // it: a trace recorded straight from an injector belongs to no universe and replays as such.
    uint64_t universe_seed() const { return universe_seed_; }
    void set_universe_seed(uint64_t seed) { universe_seed_ = seed; }

    void record(const TraceEntry& entry) {
        if (capacity_ == 0) return;
        if (entries_.size() == capacity_) {
            ++dropped_;
            return;
        }
        entries_.push_back(entry);
    }

    size_t size() const { return entries_.size(); }
    const TraceEntry& operator[](size_t index) const { return entries_[index]; }
    const TraceEntry* begin() const { return entries_.data(); }
    const TraceEntry* end() const { return entries_.data() + entries_.size(); }

  private:
    std::vector<TraceEntry> entries_{};
    uint64_t capacity_ = 0;
    uint64_t dropped_ = 0;
    uint64_t universe_seed_ = 0;
};

// Which entries a replay honours (docs/fault-injection.md §11.2). One representation — the seqs to
// withhold, sorted — behind the three shapes a caller needs.
class TraceFilter {
  public:
    TraceFilter() = default;

    static TraceFilter honour_all() { return TraceFilter{}; }

    // Exactly these entries are withheld, whatever they are.
    static TraceFilter withhold(std::vector<uint64_t> seqs) {
        TraceFilter filter;
        filter.withheld_ = std::move(seqs);
        std::sort(filter.withheld_.begin(), filter.withheld_.end());
        filter.withheld_.erase(std::unique(filter.withheld_.begin(), filter.withheld_.end()),
                               filter.withheld_.end());
        return filter;
    }

    // Every fire except the kept ones: the `--only-faults=N` repro. Entries that injected nothing
    // are never withheld — there is no fault in them to withhold.
    static TraceFilter only_faults(const FaultTrace& source, std::vector<uint64_t> kept_seqs) {
        std::sort(kept_seqs.begin(), kept_seqs.end());
        TraceFilter filter;
        for (const TraceEntry& entry : source) {
            if (entry.outcome == FaultKind::None) continue;
            if (!std::binary_search(kept_seqs.begin(), kept_seqs.end(), entry.seq)) {
                filter.withheld_.push_back(entry.seq);
            }
        }
        return filter;
    }

    bool withholds(uint64_t seq) const {
        return std::binary_search(withheld_.begin(), withheld_.end(), seq);
    }

    size_t withheld() const { return withheld_.size(); }

  private:
    std::vector<uint64_t> withheld_{};
};

} // namespace cosmos
