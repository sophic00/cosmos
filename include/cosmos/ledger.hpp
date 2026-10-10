#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/time.hpp"
#include "cosmos/trace.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace cosmos {

// §11.1 records fires, heals and limit-refused episode starts. P1 has no episodes and does not
// record gate rejections, so every P1 entry is a fire.
// fire_index counts fires only, and is deliberately not the decision trace's `seq` (§11.2), which
// numbers every post-gate decision including the ones that drew and lost. Building F5's trace on
// this counter would misalign the replayed stream, and both sequences look right while it happens.
//
// Wide fields first: the three small ones together cost 16 bytes less per entry than interleaved.
struct LedgerEntry {
    uint64_t fire_index;
    Time at;
    SiteId site;
    uint64_t eligible_index;
    FaultClass fault_class;
    FaultKind outcome;
    bool drew;
};

constexpr size_t kLedgerCapacity = 256;

// Fixed storage: decide() runs inside __wrap_malloc, so recording must never allocate (Rule 7).
// Overflow is counted rather than dropped silently.
class FaultLedger {
  public:
    void record(const LedgerEntry& entry) {
        // Absorb into the live trace digest before the capacity check: a dropped entry is an
        // event that happened, so the trace hash must reflect it even though it is not stored.
        trace_.absorb_tag(static_cast<uint8_t>(TraceEvent::FaultFire));
        trace_.absorb_u64(entry.fire_index);
        trace_.absorb_i64(entry.at.ns);
        trace_.absorb_u64(static_cast<uint64_t>(entry.site));
        trace_.absorb_u64(entry.eligible_index);
        trace_.absorb_tag(static_cast<uint8_t>(entry.fault_class));
        trace_.absorb_tag(static_cast<uint8_t>(entry.outcome));
        trace_.absorb_bool(entry.drew);

        if (size_ == kLedgerCapacity) {
            ++dropped_;
            return;
        }
        entries_[size_] = entry;
        ++size_;
    }

    size_t size() const { return size_; }
    uint64_t dropped() const { return dropped_; }
    const LedgerEntry& operator[](size_t index) const { return entries_[index]; }
    const LedgerEntry* begin() const { return entries_.data(); }
    const LedgerEntry* end() const { return entries_.data() + size_; }

    // The fault-fire portion of the universe's trace hash: every record() call, in order,
    // including the ones past capacity.
    uint64_t trace_hash() const { return trace_.digest(); }

  private:
    std::array<LedgerEntry, kLedgerCapacity> entries_{};
    size_t size_ = 0;
    uint64_t dropped_ = 0;
    TraceHash trace_{};
};

} // namespace cosmos
