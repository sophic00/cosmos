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

// The injector reads virtual time and never advances it, so a clock only has to answer now().
// P6 swaps VirtualClock's placeholder implementation for the runtime's real one; because the
// requirement is this small, neither this parameter nor decide() changes with it.
template <typename C>
concept ClockLike = requires(const C& clock) {
    { clock.now() } -> std::same_as<Time>;
};

template <ClockLike Clock> class BasicFaultInjector {
  public:
    // Construction is fallible, so it goes through a factory rather than a constructor that can
    // only assert: an injector built on an unchecked config invalidates every result of the run
    // that uses it, and an assert would wave that through under NDEBUG.
    [[nodiscard]] static std::expected<BasicFaultInjector, ConfigProblem>
    create(FaultConfig cfg, uint64_t fault_stream_seed, uint32_t node_count, const Clock& clock) {
        if (const auto checked = cfg.validate(node_count); !checked.has_value()) {
            return std::unexpected(checked.error());
        }
        for (size_t slot = 0; slot < kSiteCount; ++slot) {
            if (cfg.rules[slot].has_value() && cfg.rules[slot]->fire_on_eligible_call.has_value()) {
                return std::unexpected(
                    ConfigProblem{ConfigError::TriggerNotImplemented, site_at_slot(slot)});
            }
        }
        return BasicFaultInjector(std::move(cfg), fault_stream_seed, clock);
    }

    // Copying would fork the engine: two injectors on one clock, drawing from independent stream
    // positions and spending separate budgets against the same run. The move stays because
    // create() hands the injector back by value; a reference member rules out move-assignment.
    BasicFaultInjector(const BasicFaultInjector&) = delete;
    BasicFaultInjector& operator=(const BasicFaultInjector&) = delete;
    BasicFaultInjector(BasicFaultInjector&&) = default;
    BasicFaultInjector& operator=(BasicFaultInjector&&) = delete;

    // Gate order is the contract, not a style choice: every gate that returns before the draw is
    // what keeps a disabled fault from shifting an unrelated class's stream (§10, Rule 3).
    //
    // cls is a cross-check, not an input: every decision below reads the class off the site, so
    // wiring cls back into the logic would reintroduce two sources of truth for one fact.
    FaultKind decide(FaultClass cls, SiteId site) {
        assert(class_of(site) == cls);

        // Unknown ids and event sites have no wrapper call to decide for. Returning here also
        // keeps every index below in range without leaning on a debug-only assert.
        const size_t slot = site_slot(site);
        if (slot >= kWrapperSiteCount) return FaultKind::None;

        if (quiet_depth_ > 0) return FaultKind::None;
        if (clock_.now() >= cfg_.quiesce_after) return FaultKind::None;

        // Taken from the site rather than the caller's argument, so a mismatched class can only
        // trip the assert above, never gate on one class while drawing from another's stream.
        const FaultClass site_class = class_of(site);
        if (!cfg_.is_class_enabled(site_class)) return FaultKind::None;
        if (!cfg_.is_site_activated(site)) return FaultKind::None;
        if (clock_.now() < cfg_.warmup_until) return FaultKind::None;

        // A call is eligible once it clears quiet/class/site/warmup, and skip_first and the
        // trigger both count from here, so this increment must precede the gates below (§10).
        ++eligible_calls_[slot];

        // An activated site need not carry a rule: the swarm can turn a site on without giving it
        // anything to do, and a site with no rule can never fire.
        const FaultRule* rule = cfg_.rule_for(site);
        if (rule == nullptr) return FaultKind::None;

        if (eligible_calls_[slot] <= rule->skip_first) return FaultKind::None;
        if (injections_[slot] >= rule->max_injections) return FaultKind::None;

        return draw(site_class, slot, *rule);
    }

    // A counter rather than a flag: nested critical sections are normal, and the inner section
    // ending must not re-enable faults for the outer one.
    void push_quiet() { ++quiet_depth_; }

    // Clamped as well as asserted: an unbalanced pop that drove the depth negative would re-enable
    // faults inside a critical section that is still running.
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

    // One draw decides both whether the site fires and which outcome it produces (§6.5). A rate of
    // zero cannot fire, so it returns without drawing; a rate of one still draws, because the
    // outcome menu is what the draw selects from.
    FaultKind draw(FaultClass cls, size_t slot, const FaultRule& rule) {
        if (rule.rate <= 0.0) return FaultKind::None;

        const double value = streams_[static_cast<size_t>(cls)].uniform();
        if (value >= rule.rate) return FaultKind::None;

        double cumulative = 0.0;
        for (const SiteOutcome& outcome : rule.outcomes) {
            cumulative += outcome.weight;
            if (value / rule.rate < cumulative) {
                ++injections_[slot];
                return outcome.kind;
            }
        }
        // Normalized weights can sum to a hair under 1, leaving a draw past the last bucket.
        return FaultKind::None;
    }

    FaultConfig cfg_;
    std::array<Rng, kFaultClassCount> streams_;
    const Clock& clock_; // borrowed; the caller keeps it alive for the injector's whole life
    int quiet_depth_{0};
    SiteCounterMap eligible_calls_{};
    SiteCounterMap injections_{};
};

// Non-copyable and non-movable: a second object holding the same injector would pop a quiet window
// it never pushed, re-enabling faults inside a critical section that is still running.
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
