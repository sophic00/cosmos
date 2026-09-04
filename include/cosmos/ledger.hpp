#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/time.hpp"
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

  private:
    std::array<LedgerEntry, kLedgerCapacity> entries_{};
    size_t size_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace cosmos
