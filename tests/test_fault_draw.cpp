#include "cosmos/fault_injector.hpp"
#include "cosmos/virtual_clock.hpp"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <utility>

using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultKind;
using cosmos::FaultRule;
using cosmos::Rng;
using cosmos::select_outcome;
using cosmos::SiteId;
using cosmos::VirtualClock;

namespace {

constexpr uint32_t kNodes = 3;
constexpr uint64_t kSeed = 0x5CA1AB1E;

using Injector = cosmos::BasicFaultInjector<VirtualClock>;

constexpr void must(bool ok) { assert(ok); }

// Written pre-normalized: normalize() is not constexpr, and the edges must sit on exact binaries.
constexpr FaultRule graded_rule(double rate) {
    FaultRule rule;
    rule.rate = rate;
    must(rule.outcomes.add(FaultKind::WriteEio, 0.25));
    must(rule.outcomes.add(FaultKind::ShortWrite, 0.25));
    must(rule.outcomes.add(FaultKind::NoSpace, 0.5));
    return rule;
}

constexpr FaultRule kFullRate = graded_rule(1.0);
constexpr FaultRule kHalfRate = graded_rule(0.5);

// uniform() emits multiples of 2^-53, so -0x1p-53 is the largest draw below each edge.
static_assert(select_outcome(0.0, kFullRate) == FaultKind::WriteEio);
static_assert(select_outcome(0.25 - 0x1p-53, kFullRate) == FaultKind::WriteEio);
static_assert(select_outcome(0.25, kFullRate) == FaultKind::ShortWrite);
static_assert(select_outcome(0.5 - 0x1p-53, kFullRate) == FaultKind::ShortWrite);
static_assert(select_outcome(0.5, kFullRate) == FaultKind::NoSpace);

static_assert(select_outcome(1.0 - 0x1p-53, kFullRate) == FaultKind::NoSpace);

// Not a typo: below a power of two the exponent drops, so the nearest double is half an ulp down.
static_assert(select_outcome(0.25 - 0x1p-55, kFullRate) == FaultKind::WriteEio);
static_assert(select_outcome(0.5 - 0x1p-54, kFullRate) == FaultKind::ShortWrite);

static_assert(select_outcome(0.5, kHalfRate) == FaultKind::None);
static_assert(select_outcome(0.5 + 0x1p-53, kHalfRate) == FaultKind::None);
static_assert(select_outcome(0.5 - 0x1p-53, kHalfRate) == FaultKind::NoSpace);

static_assert(select_outcome(0.0, kHalfRate) == FaultKind::WriteEio);
static_assert(select_outcome(0.125, kHalfRate) == FaultKind::ShortWrite);
static_assert(select_outcome(0.25, kHalfRate) == FaultKind::NoSpace);

static_assert(select_outcome(0.0, graded_rule(0.0)) == FaultKind::None);

// Left unnormalized on purpose: a table summing to exactly 1.0 cannot tell `>=` from `>` at
// value == rate, because the escaped draw falls out of the walk either way.
constexpr FaultRule over_summing_rule(double rate) {
    FaultRule rule;
    rule.rate = rate;
    must(rule.outcomes.add(FaultKind::WriteEio, 0.4));
    must(rule.outcomes.add(FaultKind::ShortWrite, 0.7));
    return rule;
}

constexpr FaultRule kOverSumming = over_summing_rule(0.5);

static_assert(select_outcome(0.5, kOverSumming) == FaultKind::None);

static_assert(select_outcome(0.5 - 0x1p-53, kOverSumming) == FaultKind::ShortWrite);

FaultRule rule_with(double rate, std::initializer_list<std::pair<FaultKind, double>> outcomes) {
    FaultRule rule;
    rule.rate = rate;
    for (const auto& [kind, weight] : outcomes) {
        must(rule.outcomes.add(kind, weight));
    }
    return rule;
}

Injector make_injector(FaultConfig cfg, const VirtualClock& clock, uint64_t seed = kSeed) {
    auto injector = Injector::create(std::move(cfg), seed, kNodes, clock);
    must(injector.has_value());
    return std::move(*injector);
}

FaultConfig one_site_config(SiteId site, FaultRule rule) {
    FaultConfig cfg;
    cfg.enable_class(cosmos::class_of(site));
    must(cfg.activate_site(site));
    must(cfg.set_rule(site, std::move(rule)));
    return cfg;
}

// Seeds are fixed, so the width catches systematic error rather than absorbing flake.
bool within_sigma(long observed, long trials, double share, double sigma) {
    const double expected = static_cast<double>(trials) * share;
    const double spread = std::sqrt(static_cast<double>(trials) * share * (1.0 - share));
    return std::fabs(static_cast<double>(observed) - expected) <= sigma * spread;
}

void test_single_outcome_matches_a_plain_bernoulli() {
    constexpr double kRate = 0.3;
    constexpr int kCalls = 10000;

    VirtualClock clock;
    Injector injector = make_injector(
        one_site_config(SiteId::malloc, rule_with(kRate, {{FaultKind::OutOfMemory, 1.0}})), clock);
    Rng reference(cosmos::fault_class_seed(kSeed, FaultClass::Memory));

    for (int i = 0; i < kCalls; ++i) {
        const double value = reference.uniform();
        const FaultKind expected = value < kRate ? FaultKind::OutOfMemory : FaultKind::None;
        assert(injector.decide(FaultClass::Memory, SiteId::malloc) == expected);
    }
    assert(injector.eligible_calls(SiteId::malloc) == kCalls);

    std::cout << "[PASS] test_single_outcome_matches_a_plain_bernoulli" << std::endl;
}

// Rule 4: a walk drawing per table entry falls out of lockstep on the first call.
void test_one_draw_regardless_of_table_size() {
    constexpr int kCalls = 2000;

    for (const bool wide : {false, true}) {
        const FaultRule rule = wide ? rule_with(0.5, {{FaultKind::ConnReset, 1.0},
                                                      {FaultKind::ShortSend, 2.0},
                                                      {FaultKind::PacketDrop, 3.0},
                                                      {FaultKind::PacketDelay, 4.0},
                                                      {FaultKind::PacketReorder, 5.0},
                                                      {FaultKind::PacketCorrupt, 6.0}})
                                    : rule_with(0.5, {{FaultKind::ConnReset, 1.0}});

        VirtualClock clock;
        Injector injector = make_injector(one_site_config(SiteId::send, rule), clock);
        const FaultRule& normalized = *injector.config().rule_for(SiteId::send);
        Rng reference(cosmos::fault_class_seed(kSeed, FaultClass::Network));

        for (int i = 0; i < kCalls; ++i) {
            const FaultKind expected = select_outcome(reference.uniform(), normalized);
            assert(injector.decide(FaultClass::Network, SiteId::send) == expected);
        }
    }

    std::cout << "[PASS] test_one_draw_regardless_of_table_size" << std::endl;
}

void test_rate_calibration_over_100k_calls() {
    constexpr int kCalls = 100000;
    constexpr double kRate = 0.01;
    constexpr uint64_t kSeeds[] = {0x5CA1AB1E, 0xDEADBEEF, 0x0F1E2D3C, 0xA5A5A5A5,
                                   0x1234ABCD, 0xFEEDFACE, 0x00000001, 0x7FFFFFFF};

    long pooled = 0;
    for (uint64_t seed : kSeeds) {
        VirtualClock clock;
        Injector injector = make_injector(
            one_site_config(SiteId::write, rule_with(kRate, {{FaultKind::WriteEio, 1.0}})), clock,
            seed);

        long fires = 0;
        for (int i = 0; i < kCalls; ++i) {
            if (injector.decide(FaultClass::Storage, SiteId::write) != FaultKind::None) ++fires;
        }
        assert(injector.eligible_calls(SiteId::write) == kCalls);
        assert(injector.injections(SiteId::write) == static_cast<uint64_t>(fires));
        assert(within_sigma(fires, kCalls, kRate, 5.0));
        pooled += fires;
    }

    const long trials = kCalls * static_cast<long>(std::size(kSeeds));
    assert(within_sigma(pooled, trials, kRate, 3.0));

    std::cout << "[PASS] test_rate_calibration_over_100k_calls" << std::endl;
}

void test_outcome_distribution_matches_weights() {
    constexpr int kCalls = 100000;

    VirtualClock clock;
    Injector injector =
        make_injector(one_site_config(SiteId::write, rule_with(1.0, {{FaultKind::WriteEio, 1.0},
                                                                     {FaultKind::ShortWrite, 1.0},
                                                                     {FaultKind::NoSpace, 2.0}})),
                      clock);

    long eio = 0;
    long short_write = 0;
    long no_space = 0;
    for (int i = 0; i < kCalls; ++i) {
        switch (injector.decide(FaultClass::Storage, SiteId::write)) {
        case FaultKind::WriteEio:
            ++eio;
            break;
        case FaultKind::ShortWrite:
            ++short_write;
            break;
        case FaultKind::NoSpace:
            ++no_space;
            break;
        default:
            assert(false);
            break;
        }
    }

    assert(eio + short_write + no_space == kCalls);
    assert(within_sigma(eio, kCalls, 0.25, 5.0));
    assert(within_sigma(short_write, kCalls, 0.25, 5.0));
    assert(within_sigma(no_space, kCalls, 0.50, 5.0));

    std::cout << "[PASS] test_outcome_distribution_matches_weights" << std::endl;
}

// Not redundant: at rate 1.0 the rescale is the identity, so dropping it still passes.
void test_outcome_distribution_holds_below_full_rate() {
    constexpr int kCalls = 100000;
    constexpr double kRate = 0.1;

    VirtualClock clock;
    Injector injector =
        make_injector(one_site_config(SiteId::write, rule_with(kRate, {{FaultKind::WriteEio, 1.0},
                                                                       {FaultKind::ShortWrite, 1.0},
                                                                       {FaultKind::NoSpace, 2.0}})),
                      clock);

    long eio = 0;
    long short_write = 0;
    long no_space = 0;
    for (int i = 0; i < kCalls; ++i) {
        switch (injector.decide(FaultClass::Storage, SiteId::write)) {
        case FaultKind::None:
            break;
        case FaultKind::WriteEio:
            ++eio;
            break;
        case FaultKind::ShortWrite:
            ++short_write;
            break;
        case FaultKind::NoSpace:
            ++no_space;
            break;
        default:
            assert(false);
            break;
        }
    }

    const long fires = eio + short_write + no_space;
    assert(injector.injections(SiteId::write) == static_cast<uint64_t>(fires));
    assert(within_sigma(fires, kCalls, kRate, 5.0));
    assert(within_sigma(eio, fires, 0.25, 5.0));
    assert(within_sigma(short_write, fires, 0.25, 5.0));
    assert(within_sigma(no_space, fires, 0.50, 5.0));

    std::cout << "[PASS] test_outcome_distribution_holds_below_full_rate" << std::endl;
}

void test_every_fire_names_one_legal_outcome() {
    constexpr int kCalls = 10000;

    VirtualClock clock;
    Injector injector = make_injector(
        one_site_config(SiteId::send, rule_with(0.5, {{FaultKind::ConnReset, 1.0},
                                                      {FaultKind::ShortSend, 1.0},
                                                      {FaultKind::PacketDrop, 1.0},
                                                      {FaultKind::PacketDelay, 1.0},
                                                      {FaultKind::PacketReorder, 1.0},
                                                      {FaultKind::PacketCorrupt, 1.0}})),
        clock);

    long fires = 0;
    for (int i = 0; i < kCalls; ++i) {
        const FaultKind kind = injector.decide(FaultClass::Network, SiteId::send);
        if (kind == FaultKind::None) continue;
        assert(cosmos::is_legal_outcome(SiteId::send, kind));
        ++fires;
    }

    assert(injector.injections(SiteId::send) == static_cast<uint64_t>(fires));
    assert(injector.eligible_calls(SiteId::send) == kCalls);

    std::cout << "[PASS] test_every_fire_names_one_legal_outcome" << std::endl;
}

// The gap below 1.0 is the per-fire chance of losing a fire off the end of the walk.
// Tolerance is an empirical canary, not derived: worst case at the six-entry cap is nearer 6 eps.
void test_weight_sum_shortfall_stays_within_a_few_ulps() {
    const std::initializer_list<std::initializer_list<double>> tables = {
        {1.0, 1.0, 1.0},
        {1.0, 3.0, 7.0},
        {1.0, 1.0, 1.0, 1.0, 1.0, 1.0},
        {1e-9, 1.0, 1.0},
        {1.0, 2.0, 4.0, 8.0, 16.0, 32.0},
    };
    constexpr FaultKind kKinds[] = {FaultKind::ConnReset,     FaultKind::ShortSend,
                                    FaultKind::PacketDrop,    FaultKind::PacketDelay,
                                    FaultKind::PacketReorder, FaultKind::PacketCorrupt};
    const double tolerance = 4.0 * std::numeric_limits<double>::epsilon();

    for (const std::initializer_list<double>& weights : tables) {
        FaultRule rule;
        rule.rate = 1.0;
        size_t index = 0;
        for (double weight : weights) {
            must(rule.outcomes.add(kKinds[index], weight));
            ++index;
        }

        FaultConfig cfg = one_site_config(SiteId::send, std::move(rule));
        assert(cfg.validate(kNodes).has_value());
        cfg.normalize();

        // Left to right, the order the walk accumulates in; another order rounds differently.
        double cumulative = 0.0;
        for (const cosmos::SiteOutcome& outcome : cfg.rule_for(SiteId::send)->outcomes) {
            cumulative += outcome.weight;
        }
        assert(1.0 - cumulative <= tolerance);
        assert(cumulative - 1.0 <= tolerance);
    }

    std::cout << "[PASS] test_weight_sum_shortfall_stays_within_a_few_ulps" << std::endl;
}

void test_same_seed_replays_ten_thousand_decisions() {
    constexpr int kCalls = 10000;

    FaultConfig cfg;
    cfg.enable_class(FaultClass::Memory);
    cfg.enable_class(FaultClass::Storage);
    cfg.enable_class(FaultClass::Network);
    must(cfg.activate_site(SiteId::malloc));
    must(cfg.activate_site(SiteId::write));
    must(cfg.activate_site(SiteId::send));
    must(cfg.set_rule(SiteId::malloc, rule_with(0.4, {{FaultKind::OutOfMemory, 1.0}})));
    must(cfg.set_rule(SiteId::write, rule_with(0.6, {{FaultKind::WriteEio, 1.0},
                                                     {FaultKind::ShortWrite, 1.0},
                                                     {FaultKind::NoSpace, 2.0}})));
    must(cfg.set_rule(SiteId::send, rule_with(0.5, {{FaultKind::PacketDrop, 3.0},
                                                    {FaultKind::PacketCorrupt, 1.0}})));

    VirtualClock left_clock;
    VirtualClock right_clock;
    Injector left = make_injector(cfg, left_clock);
    Injector right = make_injector(cfg, right_clock);

    constexpr SiteId kSites[] = {SiteId::malloc, SiteId::write, SiteId::send};
    for (int i = 0; i < kCalls; ++i) {
        const SiteId site = kSites[i % 3];
        const FaultClass cls = cosmos::class_of(site);
        assert(left.decide(cls, site) == right.decide(cls, site));
    }

    for (SiteId site : kSites) {
        assert(left.eligible_calls(site) == right.eligible_calls(site));
        assert(left.injections(site) == right.injections(site));
        assert(left.injections(site) > 0);
    }

    std::cout << "[PASS] test_same_seed_replays_ten_thousand_decisions" << std::endl;
}

} // namespace

int main() {
    test_single_outcome_matches_a_plain_bernoulli();
    test_one_draw_regardless_of_table_size();
    test_rate_calibration_over_100k_calls();
    test_outcome_distribution_matches_weights();
    test_outcome_distribution_holds_below_full_rate();
    test_every_fire_names_one_legal_outcome();
    test_weight_sum_shortfall_stays_within_a_few_ulps();
    test_same_seed_replays_ten_thousand_decisions();
    std::cout << "All fault draw tests passed successfully!" << std::endl;
    return 0;
}
