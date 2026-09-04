#include "cosmos/fault_injector.hpp"
#include "cosmos/virtual_clock.hpp"
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <type_traits>
#include <utility>

using namespace cosmos::literals;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultKind;
using cosmos::FaultRule;
using cosmos::Rng;
using cosmos::SiteId;
using cosmos::VirtualClock;

namespace {

constexpr uint32_t kNodes = 3;
constexpr uint64_t kSeed = 0x5CA1AB1E;

using Injector = cosmos::BasicFaultInjector<VirtualClock>;
using Guard = cosmos::QuietGuard<Injector>;

static_assert(cosmos::ClockLike<VirtualClock>);

// A copy would pop a quiet window it never pushed, re-enabling faults mid-critical-section.
static_assert(!std::is_copy_constructible_v<Guard>);
static_assert(!std::is_copy_assignable_v<Guard>);
static_assert(!std::is_move_constructible_v<Guard>);
static_assert(!std::is_move_assignable_v<Guard>);

// A copied injector would draw and count independently while sharing the run's clock; create()
// still has to hand one back by value.
static_assert(!std::is_copy_constructible_v<Injector>);
static_assert(!std::is_copy_assignable_v<Injector>);
static_assert(std::is_move_constructible_v<Injector>);
static_assert(!std::is_move_assignable_v<Injector>);

void must(bool ok) { assert(ok); }

// Equal weights over two outcomes at rate 1.0: every eligible call fires, and the outcome names
// which half of [0,1) the single draw landed in. That makes the stream position observable through
// decide()'s return value alone, without exposing the injector's Rng state.
FaultRule two_outcome_rule(FaultKind low, FaultKind high) {
    FaultRule rule;
    rule.rate = 1.0;
    must(rule.outcomes.add(low, 1.0));
    must(rule.outcomes.add(high, 1.0));
    return rule;
}

FaultKind pick(double value, FaultKind low, FaultKind high) { return value < 0.5 ? low : high; }

// open and write are both Storage, so they share one sub-stream; read stays unactivated and malloc
// belongs to a class this config leaves disabled.
FaultConfig probe_config() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Storage);
    must(cfg.activate_site(SiteId::open));
    must(cfg.activate_site(SiteId::write));
    must(cfg.activate_site(SiteId::malloc));
    must(cfg.set_rule(SiteId::open, two_outcome_rule(FaultKind::OpenEio, FaultKind::NoSpace)));
    must(cfg.set_rule(SiteId::write, two_outcome_rule(FaultKind::WriteEio, FaultKind::ShortWrite)));
    return cfg;
}

Rng storage_reference() { return Rng(cosmos::fault_class_seed(kSeed, FaultClass::Storage)); }

Injector make_injector(FaultConfig cfg, const VirtualClock& clock) {
    auto injector = Injector::create(std::move(cfg), kSeed, kNodes, clock);
    must(injector.has_value());
    return std::move(*injector);
}

// Each decision reveals one bit of the value behind it, so a single comparison would let a
// mis-positioned stream pass half the time. Replaying a run of them makes that vanishingly small.
constexpr int kReplayLength = 12;

void expect_stream_matches(Injector& injector, Rng& reference, SiteId site, FaultKind low,
                           FaultKind high) {
    for (int i = 0; i < kReplayLength; ++i) {
        const FaultKind expected = pick(reference.uniform(), low, high);
        assert(injector.decide(FaultClass::Storage, site) == expected);
    }
}

// Replays the Storage stream from its first value: the outcomes only match if everything that
// happened before consumed no draws at all.
void expect_stream_at_start(Injector& injector) {
    Rng reference = storage_reference();
    expect_stream_matches(injector, reference, SiteId::open, FaultKind::OpenEio,
                          FaultKind::NoSpace);
}

// The open-side probe cannot be used when open itself carries the rule under test; write keeps
// probe_config()'s two-outcome rule and shares the same Storage sub-stream.
void expect_write_stream_at_start(Injector& injector) {
    Rng reference = storage_reference();
    expect_stream_matches(injector, reference, SiteId::write, FaultKind::WriteEio,
                          FaultKind::ShortWrite);
}

void test_eligible_call_draws_exactly_once() {
    VirtualClock clock;
    Injector injector = make_injector(probe_config(), clock);
    Rng reference = storage_reference();

    bool saw_low = false;
    bool saw_high = false;
    for (int i = 0; i < 8; ++i) {
        const FaultKind expected =
            pick(reference.uniform(), FaultKind::OpenEio, FaultKind::NoSpace);
        saw_low = saw_low || expected == FaultKind::OpenEio;
        saw_high = saw_high || expected == FaultKind::NoSpace;
        assert(injector.decide(FaultClass::Storage, SiteId::open) == expected);
    }
    // Without both outcomes present the sequence would match even if decide() drew twice per call.
    assert(saw_low && saw_high);

    assert(injector.eligible_calls(SiteId::open) == 8);
    assert(injector.injections(SiteId::open) == 8);

    std::cout << "[PASS] test_eligible_call_draws_exactly_once" << std::endl;
}

void test_quiet_gate_never_draws() {
    VirtualClock clock;
    Injector injector = make_injector(probe_config(), clock);

    injector.push_quiet();
    for (int i = 0; i < 100; ++i) {
        assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    }
    assert(injector.eligible_calls(SiteId::open) == 0);
    assert(injector.injections(SiteId::open) == 0);
    injector.pop_quiet();

    expect_stream_at_start(injector);

    std::cout << "[PASS] test_quiet_gate_never_draws" << std::endl;
}

void test_disabled_class_never_draws() {
    VirtualClock clock;
    Injector injector = make_injector(probe_config(), clock);

    // malloc is activated, so a zero eligible count can only mean the class gate stopped the call.
    for (int i = 0; i < 100; ++i) {
        assert(injector.decide(FaultClass::Memory, SiteId::malloc) == FaultKind::None);
    }
    assert(injector.eligible_calls(SiteId::malloc) == 0);

    expect_stream_at_start(injector);

    std::cout << "[PASS] test_disabled_class_never_draws" << std::endl;
}

void test_unactivated_site_never_draws() {
    VirtualClock clock;
    Injector injector = make_injector(probe_config(), clock);

    // read shares the Storage stream with open, so a leaked draw here would show up below.
    for (int i = 0; i < 100; ++i) {
        assert(injector.decide(FaultClass::Storage, SiteId::read) == FaultKind::None);
    }
    assert(injector.eligible_calls(SiteId::read) == 0);

    expect_stream_at_start(injector);

    std::cout << "[PASS] test_unactivated_site_never_draws" << std::endl;
}

void test_warmup_window_never_draws() {
    VirtualClock clock;
    FaultConfig cfg = probe_config();
    cfg.warmup_until = cosmos::Time::zero() + 10_ms;
    Injector injector = make_injector(std::move(cfg), clock);

    for (int i = 0; i < 100; ++i) {
        assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    }
    assert(injector.eligible_calls(SiteId::open) == 0);

    clock.advance(10_ms);
    expect_stream_at_start(injector);

    std::cout << "[PASS] test_warmup_window_never_draws" << std::endl;
}

void test_skip_first_counts_eligible_calls() {
    VirtualClock clock;
    FaultConfig cfg = probe_config();
    FaultRule rule = two_outcome_rule(FaultKind::OpenEio, FaultKind::NoSpace);
    rule.skip_first = 3;
    must(cfg.set_rule(SiteId::open, rule));
    Injector injector = make_injector(std::move(cfg), clock);

    for (uint64_t call = 1; call <= 3; ++call) {
        assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
        assert(injector.eligible_calls(SiteId::open) == call);
    }
    assert(injector.injections(SiteId::open) == 0);

    // The 4th eligible call is the first allowed to draw, so it must land on the stream's start.
    expect_stream_at_start(injector);
    assert(injector.eligible_calls(SiteId::open) == 3 + kReplayLength);
    assert(injector.injections(SiteId::open) == kReplayLength);

    std::cout << "[PASS] test_skip_first_counts_eligible_calls" << std::endl;
}

void test_spent_budget_blocks_without_drawing() {
    VirtualClock clock;
    FaultConfig cfg = probe_config();
    FaultRule rule = two_outcome_rule(FaultKind::OpenEio, FaultKind::NoSpace);
    rule.max_injections = 1;
    must(cfg.set_rule(SiteId::open, rule));
    Injector injector = make_injector(std::move(cfg), clock);

    Rng reference = storage_reference();
    const FaultKind first = pick(reference.uniform(), FaultKind::OpenEio, FaultKind::NoSpace);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == first);
    assert(injector.injections(SiteId::open) == 1);

    for (int i = 0; i < 5; ++i) {
        assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    }
    assert(injector.injections(SiteId::open) == 1);
    assert(injector.eligible_calls(SiteId::open) == 6);

    // write draws from the same Storage stream: it resumes at the second value only if the five
    // budget-blocked calls consumed nothing.
    expect_stream_matches(injector, reference, SiteId::write, FaultKind::WriteEio,
                          FaultKind::ShortWrite);

    std::cout << "[PASS] test_spent_budget_blocks_without_drawing" << std::endl;
}

void test_quiet_windows_nest() {
    VirtualClock clock;
    Injector injector = make_injector(probe_config(), clock);

    injector.push_quiet();
    injector.push_quiet();
    injector.pop_quiet();
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.eligible_calls(SiteId::open) == 0);
    injector.pop_quiet();

    Rng reference = storage_reference();
    const FaultKind first = pick(reference.uniform(), FaultKind::OpenEio, FaultKind::NoSpace);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == first);

    {
        Guard guard(injector);
        assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    }

    const FaultKind second = pick(reference.uniform(), FaultKind::OpenEio, FaultKind::NoSpace);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == second);

    std::cout << "[PASS] test_quiet_windows_nest" << std::endl;
}

void test_activated_site_without_a_rule_stays_eligible() {
    VirtualClock clock;
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Storage);
    must(cfg.activate_site(SiteId::open));
    Injector injector = make_injector(std::move(cfg), clock);

    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.eligible_calls(SiteId::open) == 1);
    assert(injector.injections(SiteId::open) == 0);

    std::cout << "[PASS] test_activated_site_without_a_rule_stays_eligible" << std::endl;
}

void test_zero_rate_never_draws() {
    VirtualClock clock;
    FaultConfig cfg = probe_config();
    FaultRule inert;
    must(cfg.set_rule(SiteId::open, inert));
    Injector injector = make_injector(std::move(cfg), clock);

    for (int i = 0; i < 100; ++i) {
        assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    }
    assert(injector.eligible_calls(SiteId::open) == 100);
    assert(injector.injections(SiteId::open) == 0);

    // Rule 3: a rate of zero cannot fire, so it must not have moved the shared Storage stream.
    Rng reference = storage_reference();
    expect_stream_matches(injector, reference, SiteId::write, FaultKind::WriteEio,
                          FaultKind::ShortWrite);

    std::cout << "[PASS] test_zero_rate_never_draws" << std::endl;
}

void test_streams_are_independent_across_classes() {
    VirtualClock clock;
    FaultConfig cfg = probe_config();
    cfg.enable_class(FaultClass::Network);
    must(cfg.activate_site(SiteId::send));
    must(cfg.set_rule(SiteId::send, two_outcome_rule(FaultKind::PacketDrop, FaultKind::ConnReset)));
    Injector injector = make_injector(std::move(cfg), clock);

    for (int i = 0; i < 16; ++i) {
        assert(injector.decide(FaultClass::Network, SiteId::send) != FaultKind::None);
    }

    // Rule 2: draining the Network stream leaves the Storage stream exactly where it was.
    expect_stream_at_start(injector);

    std::cout << "[PASS] test_streams_are_independent_across_classes" << std::endl;
}

// §14's highest-value test, in its own words: change the network config, assert the memory
// sub-stream yields the same decisions. The existing isolation test only shows that draining one
// class inside one injector leaves another alone; this shows two differently-configured runs agree.
void test_memory_decisions_survive_a_network_config_change() {
    const auto build = [](double send_rate, bool network_on) {
        FaultConfig cfg;
        cfg.enable_class(FaultClass::Memory);
        must(cfg.activate_site(SiteId::malloc));
        FaultRule oom;
        oom.rate = 0.5;
        must(oom.outcomes.add(FaultKind::OutOfMemory, 1.0));
        must(cfg.set_rule(SiteId::malloc, oom));

        if (network_on) {
            cfg.enable_class(FaultClass::Network);
            must(cfg.activate_site(SiteId::send));
            FaultRule net;
            net.rate = send_rate;
            must(net.outcomes.add(FaultKind::PacketDrop, 1.0));
            must(net.outcomes.add(FaultKind::ConnReset, 3.0));
            must(cfg.set_rule(SiteId::send, net));
        }
        return cfg;
    };

    VirtualClock quiet_clock;
    VirtualClock busy_clock;
    VirtualClock off_clock;
    Injector quiet_net = make_injector(build(0.05, true), quiet_clock);
    Injector busy_net = make_injector(build(0.95, true), busy_clock);
    Injector no_net = make_injector(build(0.0, false), off_clock);

    for (int i = 0; i < 2000; ++i) {
        const FaultKind expected = quiet_net.decide(FaultClass::Memory, SiteId::malloc);
        assert(busy_net.decide(FaultClass::Memory, SiteId::malloc) == expected);
        assert(no_net.decide(FaultClass::Memory, SiteId::malloc) == expected);

        quiet_net.decide(FaultClass::Network, SiteId::send);
        busy_net.decide(FaultClass::Network, SiteId::send);
        no_net.decide(FaultClass::Network, SiteId::send);
    }

    // The network dimension really did move, or the test above would agree for the wrong reason.
    assert(busy_net.injections(SiteId::send) > quiet_net.injections(SiteId::send));
    assert(no_net.injections(SiteId::send) == 0);
    assert(quiet_net.injections(SiteId::malloc) > 0);

    std::cout << "[PASS] test_memory_decisions_survive_a_network_config_change" << std::endl;
}

void test_quiesce_window_never_draws() {
    VirtualClock clock;
    FaultConfig cfg = probe_config();
    cfg.quiesce_after = cosmos::Time::zero() + 10_ms;
    Injector injector = make_injector(std::move(cfg), clock);

    // Faults are live before the window closes.
    assert(injector.decide(FaultClass::Storage, SiteId::open) != FaultKind::None);

    clock.advance(10_ms);
    for (int i = 0; i < 100; ++i) {
        assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    }
    assert(injector.eligible_calls(SiteId::open) == 1);
    assert(injector.injections(SiteId::open) == 1);

    // §9.3 keeps the window faults-off, and Rule 3 keeps it draw-free: the second value is still
    // waiting for whoever draws next.
    Rng reference = storage_reference();
    reference.uniform();
    clock.set(cosmos::Time::zero());
    expect_stream_matches(injector, reference, SiteId::write, FaultKind::WriteEio,
                          FaultKind::ShortWrite);

    std::cout << "[PASS] test_quiesce_window_never_draws" << std::endl;
}

void test_invalid_config_is_rejected() {
    VirtualClock clock;

    FaultConfig bad_rate = probe_config();
    FaultRule rule = two_outcome_rule(FaultKind::OpenEio, FaultKind::NoSpace);
    rule.rate = 1.5;
    must(bad_rate.set_rule(SiteId::open, rule));
    auto rejected = Injector::create(std::move(bad_rate), kSeed, kNodes, clock);
    assert(!rejected.has_value());
    assert(rejected.error().error == cosmos::ConfigError::BadRate);
    assert(rejected.error().site == SiteId::open);

    std::cout << "[PASS] test_invalid_config_is_rejected" << std::endl;
}

FaultConfig trigger_config(uint64_t on_call, FaultRule rule) {
    FaultConfig cfg = probe_config();
    rule.fire_on_eligible_call = on_call;
    must(cfg.set_rule(SiteId::open, rule));
    return cfg;
}

FaultRule storage_rule(double rate, std::initializer_list<FaultKind> kinds) {
    FaultRule rule;
    rule.rate = rate;
    double weight = 1.0;
    for (FaultKind kind : kinds) {
        must(rule.outcomes.add(kind, weight));
        weight += 1.0;
    }
    return rule;
}

void test_trigger_fires_on_its_exact_eligible_call() {
    constexpr uint64_t kOnCall = 443;

    VirtualClock clock;
    Injector injector =
        make_injector(trigger_config(kOnCall, storage_rule(0.0, {FaultKind::OpenEio})), clock);

    for (uint64_t call = 1; call <= kOnCall + 5; ++call) {
        const FaultKind expected = call == kOnCall ? FaultKind::OpenEio : FaultKind::None;
        assert(injector.decide(FaultClass::Storage, SiteId::open) == expected);
    }
    assert(injector.injections(SiteId::open) == 1);

    std::cout << "[PASS] test_trigger_fires_on_its_exact_eligible_call" << std::endl;
}

// Rule 11 on the exact case where the walk would otherwise have been needed to choose.
void test_trigger_fires_first_entry_without_drawing() {
    VirtualClock clock;
    Injector injector = make_injector(
        trigger_config(2, storage_rule(0.0, {FaultKind::NoSpace, FaultKind::OpenEio})), clock);

    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::NoSpace);

    expect_write_stream_at_start(injector);

    std::cout << "[PASS] test_trigger_fires_first_entry_without_drawing" << std::endl;
}

// Weights are a probabilistic-path concept; P4 may sample them per universe, and a scripted
// scenario must not change kind when it does.
void test_trigger_kind_is_independent_of_weights() {
    for (double heavy : {1.0, 5.0, 1000.0}) {
        FaultRule rule;
        rule.rate = 0.0;
        must(rule.outcomes.add(FaultKind::OpenEio, 1.0));
        must(rule.outcomes.add(FaultKind::NoSpace, heavy));

        VirtualClock clock;
        Injector injector = make_injector(trigger_config(1, rule), clock);
        assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::OpenEio);
    }

    std::cout << "[PASS] test_trigger_kind_is_independent_of_weights" << std::endl;
}

// Table order is the control surface for a scripted fire, the way knob order is for sampling.
void test_trigger_kind_follows_table_order() {
    VirtualClock forward_clock;
    Injector forward = make_injector(
        trigger_config(1, storage_rule(0.0, {FaultKind::OpenEio, FaultKind::NoSpace})),
        forward_clock);
    assert(forward.decide(FaultClass::Storage, SiteId::open) == FaultKind::OpenEio);

    VirtualClock reversed_clock;
    Injector reversed = make_injector(
        trigger_config(1, storage_rule(0.0, {FaultKind::NoSpace, FaultKind::OpenEio})),
        reversed_clock);
    assert(reversed.decide(FaultClass::Storage, SiteId::open) == FaultKind::NoSpace);

    std::cout << "[PASS] test_trigger_kind_follows_table_order" << std::endl;
}

// A rate-0 rule never draws, so select_outcome(0.0, rule) would return None here and the scripted
// fire would silently vanish on the most explicitly scripted config there is.
void test_rate_zero_trigger_still_fires() {
    VirtualClock clock;
    Injector injector =
        make_injector(trigger_config(3, storage_rule(0.0, {FaultKind::OpenEio})), clock);

    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::OpenEio);
    assert(injector.injections(SiteId::open) == 1);

    expect_write_stream_at_start(injector);

    std::cout << "[PASS] test_rate_zero_trigger_still_fires" << std::endl;
}

void test_trigger_counts_eligible_calls_not_raw_calls() {
    VirtualClock clock;
    Injector injector =
        make_injector(trigger_config(3, storage_rule(0.0, {FaultKind::OpenEio})), clock);

    {
        Guard guard(injector);
        for (int i = 0; i < 5; ++i) {
            assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
        }
    }
    assert(injector.eligible_calls(SiteId::open) == 0);

    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::OpenEio);

    std::cout << "[PASS] test_trigger_counts_eligible_calls_not_raw_calls" << std::endl;
}

void test_spent_budget_blocks_a_matched_trigger() {
    VirtualClock clock;
    FaultRule rule = storage_rule(0.0, {FaultKind::OpenEio});
    rule.max_injections = 0;
    Injector injector = make_injector(trigger_config(1, rule), clock);

    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.injections(SiteId::open) == 0);
    assert(injector.eligible_calls(SiteId::open) == 1);

    expect_write_stream_at_start(injector);

    std::cout << "[PASS] test_spent_budget_blocks_a_matched_trigger" << std::endl;
}

// A probabilistic fire spends the same budget a scripted one would have used.
void test_probabilistic_fire_can_exhaust_a_triggers_budget() {
    VirtualClock clock;
    FaultRule rule = two_outcome_rule(FaultKind::OpenEio, FaultKind::NoSpace);
    rule.max_injections = 1;
    Injector injector = make_injector(trigger_config(2, rule), clock);

    Rng reference = storage_reference();
    assert(injector.decide(FaultClass::Storage, SiteId::open) ==
           pick(reference.uniform(), FaultKind::OpenEio, FaultKind::NoSpace));
    assert(injector.injections(SiteId::open) == 1);

    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.injections(SiteId::open) == 1);

    std::cout << "[PASS] test_probabilistic_fire_can_exhaust_a_triggers_budget" << std::endl;
}

// The other budget direction: a scripted fire spends the budget a later probabilistic fire wanted.
void test_trigger_fire_exhausts_the_budget_for_later_calls() {
    VirtualClock clock;
    FaultRule rule = two_outcome_rule(FaultKind::OpenEio, FaultKind::NoSpace);
    rule.max_injections = 1;
    Injector injector = make_injector(trigger_config(1, rule), clock);

    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::OpenEio);
    assert(injector.injections(SiteId::open) == 1);

    // rate is 1.0, so this call would fire if the trigger had not already spent the budget.
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.injections(SiteId::open) == 1);

    expect_write_stream_at_start(injector);

    std::cout << "[PASS] test_trigger_fire_exhausts_the_budget_for_later_calls" << std::endl;
}

// The trigger call is the only one that skips the draw; the rest of the run is untouched.
void test_trigger_consumes_no_draw_mid_run() {
    VirtualClock clock;
    FaultRule rule = two_outcome_rule(FaultKind::OpenEio, FaultKind::NoSpace);
    Injector injector = make_injector(trigger_config(3, rule), clock);

    Rng reference = storage_reference();
    for (int call = 1; call <= 5; ++call) {
        const FaultKind expected =
            call == 3 ? FaultKind::OpenEio
                      : pick(reference.uniform(), FaultKind::OpenEio, FaultKind::NoSpace);
        assert(injector.decide(FaultClass::Storage, SiteId::open) == expected);
    }

    std::cout << "[PASS] test_trigger_consumes_no_draw_mid_run" << std::endl;
}

void test_trigger_below_skip_first_is_rejected() {
    VirtualClock clock;
    FaultRule rule = storage_rule(0.0, {FaultKind::OpenEio});
    rule.skip_first = 5;

    auto rejected = Injector::create(trigger_config(5, rule), kSeed, kNodes, clock);
    assert(!rejected.has_value());
    assert(rejected.error().error == cosmos::ConfigError::TriggerLeSkipFirst);
    assert(rejected.error().site == SiteId::open);

    auto accepted = Injector::create(trigger_config(6, rule), kSeed, kNodes, clock);
    assert(accepted.has_value());

    std::cout << "[PASS] test_trigger_below_skip_first_is_rejected" << std::endl;
}

void test_event_sites_decide_nothing() {
    VirtualClock clock;
    FaultConfig cfg = probe_config();
    cfg.enable_class(FaultClass::Process);
    must(cfg.activate_site(SiteId::crash_node));
    Injector injector = make_injector(std::move(cfg), clock);

    // Episodes have no wrapper call to count or decide for; they fire via scheduled_episodes.
    for (int i = 0; i < 16; ++i) {
        assert(injector.decide(FaultClass::Process, SiteId::crash_node) == FaultKind::None);
    }
    assert(injector.eligible_calls(SiteId::crash_node) == 0);
    assert(injector.injections(SiteId::crash_node) == 0);

    std::cout << "[PASS] test_event_sites_decide_nothing" << std::endl;
}

void test_same_seed_replays_identically() {
    VirtualClock left_clock;
    VirtualClock right_clock;
    Injector left = make_injector(probe_config(), left_clock);
    Injector right = make_injector(probe_config(), right_clock);

    for (int i = 0; i < 200; ++i) {
        const SiteId site = i % 3 == 0 ? SiteId::write : SiteId::open;
        assert(left.decide(FaultClass::Storage, site) == right.decide(FaultClass::Storage, site));
    }

    for (SiteId site : {SiteId::open, SiteId::write}) {
        assert(left.eligible_calls(site) == right.eligible_calls(site));
        assert(left.injections(site) == right.injections(site));
    }

    std::cout << "[PASS] test_same_seed_replays_identically" << std::endl;
}

} // namespace

int main() {
    test_eligible_call_draws_exactly_once();
    test_quiet_gate_never_draws();
    test_disabled_class_never_draws();
    test_unactivated_site_never_draws();
    test_warmup_window_never_draws();
    test_skip_first_counts_eligible_calls();
    test_spent_budget_blocks_without_drawing();
    test_quiet_windows_nest();
    test_activated_site_without_a_rule_stays_eligible();
    test_zero_rate_never_draws();
    test_streams_are_independent_across_classes();
    test_memory_decisions_survive_a_network_config_change();
    test_quiesce_window_never_draws();
    test_invalid_config_is_rejected();
    test_trigger_fires_on_its_exact_eligible_call();
    test_trigger_fires_first_entry_without_drawing();
    test_trigger_kind_is_independent_of_weights();
    test_trigger_kind_follows_table_order();
    test_rate_zero_trigger_still_fires();
    test_trigger_counts_eligible_calls_not_raw_calls();
    test_spent_budget_blocks_a_matched_trigger();
    test_probabilistic_fire_can_exhaust_a_triggers_budget();
    test_trigger_fire_exhausts_the_budget_for_later_calls();
    test_trigger_consumes_no_draw_mid_run();
    test_trigger_below_skip_first_is_rejected();
    test_event_sites_decide_nothing();
    test_same_seed_replays_identically();
    std::cout << "All fault injector tests passed successfully!" << std::endl;
    return 0;
}
