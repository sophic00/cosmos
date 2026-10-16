#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/ledger.hpp"
#include "cosmos/random.hpp"
#include "cosmos/time.hpp"
#include "cosmos/trace.hpp"
#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <expected>
#include <utility>

namespace cosmos {

// P6 swaps VirtualClock's implementation, not this parameter: the injector only ever reads now().
template <typename C>
concept ClockLike = requires(const C& clock) {
    { clock.now() } -> std::same_as<Time>;
};

// Pure so the edges can be pinned at values no seed reaches; rate 0 returns before dividing.
constexpr FaultKind select_outcome(double value, const FaultRule& rule) {
    if (value >= rule.rate) return FaultKind::None;

    const double rescaled = value / rule.rate;
    double cumulative = 0.0;
    for (const SiteOutcome& outcome : rule.outcomes) {
        cumulative += outcome.weight;
        if (rescaled < cumulative) return outcome.kind;
    }
    // Normalized weights can sum to a hair under 1, leaving a draw past the last bucket.
    return FaultKind::None;
}

// Rate 0 cannot fire so it never draws (Rule 3); rate 1 still draws, to pick the outcome.
constexpr bool consumes_draw(const FaultRule& rule) { return rule.rate > 0.0; }

template <ClockLike Clock> class BasicFaultInjector {
  public:
    // A factory, not a constructor: an assert would wave an invalid config through under NDEBUG.
    [[nodiscard]] static std::expected<BasicFaultInjector, ConfigProblem>
    create(FaultConfig cfg, uint64_t fault_stream_seed, uint32_t node_count, const Clock& clock,
           TraceOptions trace_options = {}) {
        if (const auto checked = cfg.validate(node_count); !checked.has_value()) {
            return std::unexpected(checked.error());
        }
        // An enabled trace with no room is a configuration mistake, not a silently empty trace.
        if (trace_options.enabled && trace_options.capacity == 0) {
            return std::unexpected(ConfigProblem{ConfigError::BadTraceCapacity, std::nullopt});
        }
        return BasicFaultInjector(std::move(cfg), fault_stream_seed, clock, trace_options);
    }

    // A copy would fork the engine: two stream positions and two budgets against one run.
    BasicFaultInjector(const BasicFaultInjector&) = delete;
    BasicFaultInjector& operator=(const BasicFaultInjector&) = delete;
    BasicFaultInjector(BasicFaultInjector&&) = default;
    BasicFaultInjector& operator=(BasicFaultInjector&&) = delete;

    // Gate order is the contract: each early return is what keeps a dead fault from shifting an
    // unrelated class's stream (§10, Rule 3). cls is a cross-check only; the site names the class.
    FaultKind decide([[maybe_unused]] FaultClass cls, SiteId site) {
        assert(class_of(site) == cls);

        // Event sites have no wrapper call, and this keeps every index below in range.
        const size_t slot = site_slot(site);
        if (slot >= kWrapperSiteCount) return FaultKind::None;

        if (quiet_depth_ > 0) return FaultKind::None;
        if (clock_.now() >= cfg_.quiesce_after) return FaultKind::None;

        // From the site, never the argument: gating on one class and drawing from another's
        // stream would survive NDEBUG.
        const FaultClass site_class = class_of(site);
        if (!cfg_.is_class_enabled(site_class)) return FaultKind::None;
        if (!cfg_.is_site_activated(site)) return FaultKind::None;
        if (clock_.now() < cfg_.warmup_until) return FaultKind::None;

        // skip_first and the trigger count from here, so this precedes the gates below (§10).
        ++eligible_calls_[slot];

        // The swarm can activate a site without giving it a rule.
        const FaultRule* rule = cfg_.rule_for(site);
        if (rule == nullptr) return FaultKind::None;

        if (eligible_calls_[slot] <= rule->skip_first) return FaultKind::None;
        if (injections_[slot] >= rule->max_injections) return FaultKind::None;

        if (replaying()) return replay_decide(site_class, site, slot, *rule);

        // Entry 0 is the walk's zero point: every weight is positive, so a draw approaching 0
        // always lands there. Reading it directly is what keeps the fire zero-draw (Rule 11) —
        // passing 0.0 to select_outcome() would return None for the legal rate-0 trigger-only rule.
        if (rule->fire_on_eligible_call == eligible_calls_[slot]) {
            const FaultKind kind = rule->outcomes.entries[0].kind;
            record_decision(site, slot, *rule, kind, false);
            return record_fire(site_class, site, slot, kind, false);
        }

        const FaultKind kind = draw(site_class, *rule);
        record_decision(site, slot, *rule, kind, consumes_draw(*rule));
        if (kind == FaultKind::None) return FaultKind::None;
        return record_fire(site_class, site, slot, kind, true);
    }

    // A counter, not a flag: an inner critical section ending must not re-enable the outer one.
    void push_quiet() { ++quiet_depth_; }

    // Clamped as well as asserted: underflow re-enables faults inside a live critical section.
    void pop_quiet() {
        assert(quiet_depth_ > 0);
        if (quiet_depth_ > 0) --quiet_depth_;
    }

    // Outcome weights read back normalized to sum to 1, not as the caller wrote them.
    const FaultConfig& config() const { return cfg_; }

    uint64_t eligible_calls(SiteId site) const {
        const size_t slot = site_slot(site);
        return slot == kNoSite ? 0 : eligible_calls_[slot];
    }

    uint64_t injections(SiteId site) const {
        const size_t slot = site_slot(site);
        return slot == kNoSite ? 0 : injections_[slot];
    }

    const FaultLedger& ledger() const { return ledger_; }

    const FaultTrace& trace() const { return trace_; }
    uint64_t decisions() const { return decisions_; }

    // Every draw in the engine goes through one place, so this counts the stream's exact advance.
    uint64_t draws() const { return draws_; }

    // §11.2's replay input: honour this schedule, withholding whatever the filter names. The
    // caller keeps the trace alive; the injector only reads it.
    void replay_from(const FaultTrace& source, TraceFilter filter) {
        source_ = &source;
        filter_ = std::move(filter);
        cursor_ = 0;
    }

    bool replaying() const { return source_ != nullptr; }
    uint64_t replay_consumed() const { return cursor_; }
    uint64_t replay_withheld() const { return withheld_; }

    // The universe names its own trace, so a replay cannot be seeded against the wrong run.
    void set_universe_seed(uint64_t seed) { trace_.set_universe_seed(seed); }

  private:
    BasicFaultInjector(FaultConfig cfg, uint64_t fault_stream_seed, const Clock& clock,
                       TraceOptions trace_options)
        : cfg_(std::move(cfg)),
          streams_(make_streams(fault_stream_seed, std::make_index_sequence<kFaultClassCount>{})),
          clock_(clock),
          // Sized once, while no universe is current: the buffer is not tracked and not faulted.
          trace_(trace_options.enabled ? trace_options.capacity : 0) {
        cfg_.normalize();
    }

    template <size_t... I>
    static std::array<Rng, sizeof...(I)> make_streams(uint64_t seed, std::index_sequence<I...>) {
        return {Rng(fault_class_seed(seed, static_cast<FaultClass>(I)))...};
    }

    // Rate 0 cannot fire so it never draws (Rule 3); rate 1 still draws, to pick the outcome.
    FaultKind draw(FaultClass cls, const FaultRule& rule) {
        if (!consumes_draw(rule)) return FaultKind::None;
        return select_outcome(next_value(cls), rule);
    }

    // The one place a class stream advances, so draws() and the stream cannot disagree.
    double next_value(FaultClass cls) {
        ++draws_;
        return streams_[static_cast<size_t>(cls)].uniform();
    }

    const TraceEntry* peek() const {
        if (source_ == nullptr || cursor_ >= source_->size()) return nullptr;
        return &(*source_)[cursor_];
    }

    // §11.2: one cursor. A site match consumes the entry; anything else passes through untouched,
    // because a replay never invents a fault the source run did not have (Rule 12).
    FaultKind replay_decide(FaultClass cls, SiteId site, size_t slot, const FaultRule& rule) {
        const TraceEntry* entry = peek();
        if (entry == nullptr || entry->site != site) {
            record_decision(site, slot, rule, FaultKind::None, false);
            return FaultKind::None;
        }

        ++cursor_;
        // Consumed even when withheld: the stream has to end where the source run left it.
        if (entry->drew) (void)next_value(cls);

        if (filter_.withholds(entry->seq)) {
            record_withheld(cls, site, slot, *entry);
            record_decision(site, slot, rule, entry->outcome, entry->drew,
                            DecisionStatus::Suppressed);
            return FaultKind::None;
        }

        record_decision(site, slot, rule, entry->outcome, entry->drew);
        if (entry->outcome == FaultKind::None) return FaultKind::None;
        return record_fire(cls, site, slot, entry->outcome, entry->drew);
    }

    // §11.1 shows what the application was spared, so a withheld fire is a ledger row of its own.
    // It takes no fire_index — nothing fired, so the field names the index it would have had.
    void record_withheld(FaultClass cls, SiteId site, size_t slot, const TraceEntry& entry) {
        ++withheld_;
        ledger_.record(LedgerEntry{.fire_index = fires_,
                                   .at = clock_.now(),
                                   .site = site,
                                   .eligible_index = eligible_calls_[slot],
                                   .fault_class = cls,
                                   .outcome = entry.outcome,
                                   .drew = entry.drew,
                                   .status = DecisionStatus::Suppressed});
    }

    // One entry per post-gate decision, numbered by the trace's own seq, never by fire_index. The
    // counter runs whether or not the trace does, so decisions() stays the decision count.
    void record_decision(SiteId site, size_t slot, const FaultRule& rule, FaultKind kind, bool drew,
                         DecisionStatus status = DecisionStatus::Fired) {
        ++decisions_;
        if (!trace_.enabled()) return;
        trace_.record(TraceEntry{.seq = decisions_,
                                 .eligible_index = eligible_calls_[slot],
                                 .at = clock_.now(),
                                 .site = site,
                                 .amount = rule.amount,
                                 .outcome = kind,
                                 .drew = drew,
                                 .status = status});
    }

    // Both fire paths land here, so the counter and the ledger cannot drift apart.
    FaultKind record_fire(FaultClass cls, SiteId site, size_t slot, FaultKind kind, bool drew) {
        ++injections_[slot];
        ledger_.record(LedgerEntry{.fire_index = fires_,
                                   .at = clock_.now(),
                                   .site = site,
                                   .eligible_index = eligible_calls_[slot],
                                   .fault_class = cls,
                                   .outcome = kind,
                                   .drew = drew});
        ++fires_;
        return kind;
    }

    FaultConfig cfg_;
    std::array<Rng, kFaultClassCount> streams_;
    const Clock& clock_; // borrowed; the caller keeps it alive for the injector's whole life
    int quiet_depth_{0};
    SiteCounterMap eligible_calls_{};
    SiteCounterMap injections_{};
    FaultLedger ledger_{};
    uint64_t fires_{0};
    FaultTrace trace_{};
    uint64_t decisions_{0};
    uint64_t draws_{0};
    const FaultTrace* source_{nullptr};
    TraceFilter filter_{};
    size_t cursor_{0};
    uint64_t withheld_{0};
};

// Non-movable too: a second owner would pop a quiet window it never pushed.
template <typename Injector> class QuietGuard {
  public:
    explicit QuietGuard(Injector& injector) : injector_(injector) { injector_.push_quiet(); }
    ~QuietGuard() { injector_.pop_quiet(); }

    QuietGuard(const QuietGuard&) = delete;
    QuietGuard& operator=(const QuietGuard&) = delete;
    QuietGuard(QuietGuard&&) = delete;
    QuietGuard& operator=(QuietGuard&&) = delete;

  private:
    Injector& injector_;
};

} // namespace cosmos
