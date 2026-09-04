#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/random.hpp"
#include "cosmos/time.hpp"
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

template <ClockLike Clock> class BasicFaultInjector {
  public:
    // A factory, not a constructor: an assert would wave an invalid config through under NDEBUG.
    [[nodiscard]] static std::expected<BasicFaultInjector, ConfigProblem>
    create(FaultConfig cfg, uint64_t fault_stream_seed, uint32_t node_count, const Clock& clock) {
        if (const auto checked = cfg.validate(node_count); !checked.has_value()) {
            return std::unexpected(checked.error());
        }
        return BasicFaultInjector(std::move(cfg), fault_stream_seed, clock);
    }

    // A copy would fork the engine: two stream positions and two budgets against one run.
    BasicFaultInjector(const BasicFaultInjector&) = delete;
    BasicFaultInjector& operator=(const BasicFaultInjector&) = delete;
    BasicFaultInjector(BasicFaultInjector&&) = default;
    BasicFaultInjector& operator=(BasicFaultInjector&&) = delete;

    // Gate order is the contract: each early return is what keeps a dead fault from shifting an
    // unrelated class's stream (§10, Rule 3). cls is a cross-check only; the site names the class.
    FaultKind decide(FaultClass cls, SiteId site) {
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

        // Entry 0 is the walk's zero point: every weight is positive, so a draw approaching 0
        // always lands there. Reading it directly is what keeps the fire zero-draw (Rule 11) —
        // passing 0.0 to select_outcome() would return None for the legal rate-0 trigger-only rule.
        if (rule->fire_on_eligible_call == eligible_calls_[slot]) {
            ++injections_[slot];
            return rule->outcomes.entries[0].kind;
        }

        return draw(site_class, slot, *rule);
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

  private:
    BasicFaultInjector(FaultConfig cfg, uint64_t fault_stream_seed, const Clock& clock)
        : cfg_(std::move(cfg)),
          streams_(make_streams(fault_stream_seed, std::make_index_sequence<kFaultClassCount>{})),
          clock_(clock) {
        cfg_.normalize();
    }

    template <size_t... I>
    static std::array<Rng, sizeof...(I)> make_streams(uint64_t seed, std::index_sequence<I...>) {
        return {Rng(fault_class_seed(seed, static_cast<FaultClass>(I)))...};
    }

    // Rate 0 cannot fire so it never draws (Rule 3); rate 1 still draws, to pick the outcome.
    FaultKind draw(FaultClass cls, size_t slot, const FaultRule& rule) {
        if (rule.rate <= 0.0) return FaultKind::None;

        const FaultKind kind = select_outcome(streams_[static_cast<size_t>(cls)].uniform(), rule);
        if (kind != FaultKind::None) ++injections_[slot];
        return kind;
    }

    FaultConfig cfg_;
    std::array<Rng, kFaultClassCount> streams_;
    const Clock& clock_; // borrowed; the caller keeps it alive for the injector's whole life
    int quiet_depth_{0};
    SiteCounterMap eligible_calls_{};
    SiteCounterMap injections_{};
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
