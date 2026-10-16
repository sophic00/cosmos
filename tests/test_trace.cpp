#include "cosmos/fault_injector.hpp"
#include "cosmos/scenario.hpp"
#include "cosmos/virtual_clock.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>

using namespace cosmos::literals;
using cosmos::DecisionStatus;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultKind;
using cosmos::FaultRule;
using cosmos::SiteId;
using cosmos::TraceEntry;
using cosmos::TraceOptions;
using cosmos::VirtualClock;

namespace {

constexpr uint32_t kNodes = 3;
constexpr uint64_t kSeed = 0x5CA1AB1E;

using Injector = cosmos::BasicFaultInjector<VirtualClock>;

void must(bool ok) { assert(ok); }

TraceOptions recording(uint64_t capacity = cosmos::kDefaultTraceCapacity) {
    return TraceOptions{.enabled = true, .capacity = capacity};
}

Injector make_injector(FaultConfig cfg, const VirtualClock& clock, TraceOptions options) {
    auto injector = Injector::create(std::move(cfg), kSeed, kNodes, clock, options);
    must(injector.has_value());
    return std::move(*injector);
}

FaultRule rule_with(double rate, FaultKind kind) {
    FaultRule rule;
    rule.rate = rate;
    must(rule.outcomes.add(kind, 1.0));
    return rule;
}

FaultConfig one_site(SiteId site, FaultRule rule) {
    FaultConfig cfg;
    cfg.enable_class(cosmos::class_of(site));
    must(cfg.activate_site(site));
    must(cfg.set_rule(site, std::move(rule)));
    return cfg;
}

// Field-wise: the trace is what a replay consumes, so its values are the comparison.
bool same_trace(const cosmos::FaultTrace& left, const cosmos::FaultTrace& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (!(left[index] == right[index])) return false;
    }
    return true;
}

void test_trace_is_off_until_enabled() {
    VirtualClock clock;
    Injector injector =
        make_injector(one_site(SiteId::write, rule_with(1.0, FaultKind::WriteEio)), clock, {});

    assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::WriteEio);
    assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::WriteEio);

    assert(!injector.trace().enabled());
    assert(injector.trace().size() == 0);
    assert(injector.trace().dropped() == 0);
    // Both calls were still decisions: the switch turns recording off, not deciding.
    assert(injector.decisions() == 2);
    // Off means off: the counters and the ledger are untouched by the switch.
    assert(injector.eligible_calls(SiteId::write) == 2);
    assert(injector.ledger().size() == 2);

    std::cout << "[PASS] test_trace_is_off_until_enabled" << std::endl;
}

void test_entries_carry_site_time_amount_and_status() {
    VirtualClock clock;
    clock.advance(12_ms);

    FaultRule rule = rule_with(1.0, FaultKind::WriteEio);
    rule.amount = 7_ms;
    Injector injector = make_injector(one_site(SiteId::write, std::move(rule)), clock, recording());

    assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::WriteEio);
    assert(injector.trace().size() == 1);

    const TraceEntry& entry = injector.trace()[0];
    assert(entry.seq == 1);
    assert(entry.at == cosmos::Time::zero() + 12_ms);
    assert(entry.site == SiteId::write);
    assert(entry.eligible_index == 1);
    assert(entry.amount == 7_ms);
    assert(entry.outcome == FaultKind::WriteEio);
    assert(entry.drew);
    assert(entry.status == DecisionStatus::Fired);

    std::cout << "[PASS] test_entries_carry_site_time_amount_and_status" << std::endl;
}

// seq numbers every post-gate decision, which is what a replay aligns on (§11.2).
void test_mixed_decisions_are_recorded_in_seq_order() {
    VirtualClock clock;
    FaultConfig cfg = one_site(SiteId::write, rule_with(1.0, FaultKind::WriteEio));
    must(cfg.activate_site(SiteId::open));
    FaultRule scripted = rule_with(0.0, FaultKind::OpenEio);
    scripted.fire_on_eligible_call = 2;
    must(cfg.set_rule(SiteId::open, std::move(scripted)));
    Injector injector = make_injector(std::move(cfg), clock, recording());

    assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::WriteEio);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::OpenEio);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);

    assert(injector.trace().size() == 4);
    assert(injector.decisions() == 4);
    for (uint64_t index = 0; index < 4; ++index) {
        assert(injector.trace()[index].seq == index + 1);
    }

    assert(injector.trace()[0].site == SiteId::write);
    assert(injector.trace()[0].drew);
    assert(injector.trace()[0].outcome == FaultKind::WriteEio);

    // A trigger fire consumes no draw, but the calls that missed the trigger are still decisions.
    assert(injector.trace()[1].site == SiteId::open);
    assert(!injector.trace()[1].drew);
    assert(injector.trace()[1].outcome == FaultKind::None);
    assert(injector.trace()[2].site == SiteId::open);
    assert(!injector.trace()[2].drew);
    assert(injector.trace()[2].outcome == FaultKind::OpenEio);
    assert(injector.trace()[3].site == SiteId::open);
    assert(!injector.trace()[3].drew);

    std::cout << "[PASS] test_mixed_decisions_are_recorded_in_seq_order" << std::endl;
}

// The §11.1/§11.2 split, measured: the same run writes one ledger entry per fire and one trace
// entry per decision, so a rate-driven run has thousands of drew-and-lost entries the ledger lacks.
void test_drew_and_lost_entries_outnumber_the_ledger() {
    constexpr uint64_t kCalls = 5000;

    VirtualClock clock;
    Injector injector = make_injector(
        one_site(SiteId::malloc, rule_with(0.001, FaultKind::OutOfMemory)), clock, recording());

    for (uint64_t call = 0; call < kCalls; ++call) {
        (void)injector.decide(FaultClass::Memory, SiteId::malloc);
    }

    assert(injector.trace().size() == kCalls);
    assert(injector.decisions() == kCalls);
    assert(injector.trace().dropped() == 0);

    uint64_t lost = 0;
    for (const TraceEntry& entry : injector.trace()) {
        assert(entry.site == SiteId::malloc);
        assert(entry.drew);
        if (entry.outcome == FaultKind::None) ++lost;
    }
    assert(lost == kCalls - injector.injections(SiteId::malloc));
    assert(lost > 0);
    assert(injector.ledger().size() == injector.injections(SiteId::malloc));

    std::cout << "[PASS] test_drew_and_lost_entries_outnumber_the_ledger" << std::endl;
}

// A trigger-only rule draws nothing at all, yet each of its calls is still a decision a replay
// has to step over — omitting them would let the cursor slide and honour a later site's entry.
void test_zero_draw_passes_are_recorded() {
    VirtualClock clock;
    FaultRule scripted = rule_with(0.0, FaultKind::OpenEio);
    scripted.fire_on_eligible_call = 5;
    Injector injector =
        make_injector(one_site(SiteId::open, std::move(scripted)), clock, recording());

    for (uint64_t call = 0; call < 10; ++call) {
        const FaultKind kind = injector.decide(FaultClass::Storage, SiteId::open);
        assert(kind == (call == 4 ? FaultKind::OpenEio : FaultKind::None));
    }

    assert(injector.trace().size() == 10);
    for (uint64_t call = 0; call < 10; ++call) {
        const TraceEntry& entry = injector.trace()[call];
        assert(!entry.drew);
        assert(entry.eligible_index == call + 1);
        assert(entry.outcome == (call == 4 ? FaultKind::OpenEio : FaultKind::None));
    }

    std::cout << "[PASS] test_zero_draw_passes_are_recorded" << std::endl;
}

void test_gate_blocked_calls_are_not_recorded() {
    // Quiet window: fires off, so nothing reaches the boundary.
    {
        VirtualClock clock;
        Injector injector = make_injector(
            one_site(SiteId::open, rule_with(1.0, FaultKind::OpenEio)), clock, recording());
        cosmos::QuietGuard<Injector> guard(injector);
        assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
        assert(injector.trace().size() == 0);
        assert(injector.eligible_calls(SiteId::open) == 0);
    }
    // A disabled class, and an event site, which has no wrapper call to decide at all.
    {
        VirtualClock clock;
        Injector injector = make_injector(
            one_site(SiteId::open, rule_with(1.0, FaultKind::OpenEio)), clock, recording());
        assert(injector.decide(FaultClass::Memory, SiteId::malloc) == FaultKind::None);
        assert(injector.decide(FaultClass::Process, SiteId::crash_node) == FaultKind::None);
        assert(injector.trace().size() == 0);
    }
    // The class is on but this site was never armed: gated, and not even eligible.
    {
        VirtualClock clock;
        FaultConfig cfg;
        cfg.enable_class(FaultClass::Memory);
        Injector injector = make_injector(std::move(cfg), clock, recording());
        assert(injector.decide(FaultClass::Memory, SiteId::malloc) == FaultKind::None);
        assert(injector.trace().size() == 0);
        assert(injector.eligible_calls(SiteId::malloc) == 0);
    }
    {
        VirtualClock clock;
        FaultConfig cfg;
        cfg.enable_class(FaultClass::Memory);
        must(cfg.activate_site(SiteId::malloc));
        // An activated site with no rule is still a gate instead of a decision (no draw possible).
        Injector injector = make_injector(std::move(cfg), clock, recording());
        assert(injector.decide(FaultClass::Memory, SiteId::malloc) == FaultKind::None);
        assert(injector.trace().size() == 0);
        assert(injector.eligible_calls(SiteId::malloc) == 1);
    }
    // Warmup, skip_first and a spent budget each return before the boundary, and each still counts
    // the call as eligible — the counters move, the trace stays empty.
    {
        VirtualClock clock;
        FaultConfig cfg = one_site(SiteId::write, rule_with(1.0, FaultKind::WriteEio));
        cfg.warmup_until = cosmos::Time::zero() + 10_ms;
        Injector injector = make_injector(std::move(cfg), clock, recording());
        assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::None);
        assert(injector.trace().size() == 0);
        clock.advance(10_ms);
        assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::WriteEio);
        assert(injector.trace().size() == 1);
    }
    {
        VirtualClock clock;
        FaultRule rule = rule_with(1.0, FaultKind::WriteEio);
        rule.skip_first = 3;
        Injector injector =
            make_injector(one_site(SiteId::write, std::move(rule)), clock, recording());
        for (uint64_t call = 0; call < 4; ++call) {
            (void)injector.decide(FaultClass::Storage, SiteId::write);
        }
        assert(injector.trace().size() == 1);
        assert(injector.trace()[0].eligible_index == 4);
    }
    {
        VirtualClock clock;
        FaultRule rule = rule_with(1.0, FaultKind::WriteEio);
        rule.max_injections = 0;
        Injector injector =
            make_injector(one_site(SiteId::write, std::move(rule)), clock, recording());
        assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::None);
        assert(injector.trace().size() == 0);
        assert(injector.eligible_calls(SiteId::write) == 1);
    }
    // Quiesce: past the deadline every call is gated, and nothing more is recorded.
    {
        VirtualClock clock;
        FaultConfig cfg = one_site(SiteId::write, rule_with(1.0, FaultKind::WriteEio));
        cfg.quiesce_after = cosmos::Time::zero() + 5_ms;
        Injector injector = make_injector(std::move(cfg), clock, recording());
        clock.advance(5_ms);
        assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::None);
        assert(injector.trace().size() == 0);
    }

    std::cout << "[PASS] test_gate_blocked_calls_are_not_recorded" << std::endl;
}

void test_same_seed_gives_an_identical_trace() {
    // Returns the trace, not the injector: the injector borrows its clock, which is local here.
    const auto drive = [](uint64_t seed) {
        VirtualClock clock;
        FaultConfig cfg = one_site(SiteId::write, rule_with(0.3, FaultKind::WriteEio));
        must(cfg.activate_site(SiteId::open));
        must(cfg.set_rule(SiteId::open, rule_with(0.4, FaultKind::OpenEio)));
        auto injector = Injector::create(std::move(cfg), seed, kNodes, clock, recording());
        must(injector.has_value());
        for (uint64_t call = 0; call < 200; ++call) {
            (void)injector->decide(FaultClass::Storage,
                                   call % 2 == 0 ? SiteId::write : SiteId::open);
        }
        return injector->trace();
    };

    const cosmos::FaultTrace left = drive(kSeed);
    const cosmos::FaultTrace right = drive(kSeed);
    assert(same_trace(left, right));
    assert(left.size() == 200);

    const cosmos::FaultTrace other = drive(kSeed + 1);
    assert(!same_trace(left, other));

    std::cout << "[PASS] test_same_seed_gives_an_identical_trace" << std::endl;
}

void test_trace_overflow_is_counted() {
    VirtualClock clock;
    Injector injector = make_injector(one_site(SiteId::write, rule_with(1.0, FaultKind::WriteEio)),
                                      clock, recording(4));

    for (uint64_t call = 0; call < 10; ++call) {
        (void)injector.decide(FaultClass::Storage, SiteId::write);
    }

    assert(injector.trace().size() == 4);
    assert(injector.trace().dropped() == 6);
    assert(injector.decisions() == 10);
    assert(injector.trace()[3].seq == 4);
    // Counters stay exact across overflow, so the report can still say what happened.
    assert(injector.eligible_calls(SiteId::write) == 10);
    assert(injector.injections(SiteId::write) == 10);

    std::cout << "[PASS] test_trace_overflow_is_counted" << std::endl;
}

// The checkpoint's scale: one entry per post-gate decision, and memory that follows the decisions
// rather than the universe count.
void test_a_million_decisions_are_all_recorded() {
    constexpr uint64_t kCalls = 1'000'000;

    VirtualClock clock;
    Injector injector = make_injector(
        one_site(SiteId::malloc, rule_with(1.0, FaultKind::OutOfMemory)), clock, recording());

    for (uint64_t call = 0; call < kCalls; ++call) {
        assert(injector.decide(FaultClass::Memory, SiteId::malloc) == FaultKind::OutOfMemory);
    }

    assert(injector.trace().size() == kCalls);
    assert(injector.trace().dropped() == 0);
    assert(injector.decisions() == kCalls);
    assert(injector.eligible_calls(SiteId::malloc) == kCalls);
    assert(injector.trace()[kCalls - 1].seq == kCalls);

    std::cout << "[PASS] test_a_million_decisions_are_all_recorded" << std::endl;
}

// A harness calls decide() directly, where the wrapper guard is not on the stack: if recording
// itself allocated, the engine's block would land in the tracked heap and re-enter decide(). Only
// the static-libstdc++ twin can measure it — see the CMakeLists note on test_trace_static.
void test_recording_never_allocates_with_a_universe_current() {
    constexpr uint64_t kCalls = 64;

    cosmos::Simulator sim(kSeed);
    must(sim.install_faults(one_site(SiteId::malloc, rule_with(0.0, FaultKind::OutOfMemory)), 1,
                            recording(1024))
             .has_value());
    auto* injector = sim.injector_or_null();
    must(injector != nullptr);

    cosmos::Simulator::set_current(&sim);
    for (uint64_t call = 0; call < kCalls; ++call) {
        assert(injector->decide(FaultClass::Memory, SiteId::malloc) == FaultKind::None);
    }
    const size_t live = sim.heap().active_count();
    cosmos::Simulator::set_current(nullptr);

    assert(live == 0);
    assert(injector->trace().size() == kCalls);
    assert(injector->decisions() == kCalls);
    assert(injector->eligible_calls(SiteId::malloc) == kCalls);
    assert(injector->ledger().size() == 0);

    std::cout << "[PASS] test_recording_never_allocates_with_a_universe_current" << std::endl;
}

// A caller that asks for a trace and leaves no room gets a refusal, not a silent empty trace.
void test_an_enabled_trace_with_no_capacity_is_rejected() {
    VirtualClock clock;
    auto injector =
        Injector::create(one_site(SiteId::write, rule_with(1.0, FaultKind::WriteEio)), kSeed,
                         kNodes, clock, TraceOptions{.enabled = true, .capacity = 0});

    must(!injector.has_value());
    assert(injector.error().error == cosmos::ConfigError::BadTraceCapacity);

    std::cout << "[PASS] test_an_enabled_trace_with_no_capacity_is_rejected" << std::endl;
}

// The buffer is sized before the universe is current, so it never reaches the tracked heap: the
// leak oracle must not see it, or every traced run would report a leak of its own apparatus.
// The static-libstdc++ twin is the link where that is measurable, not merely un-contradicted.
void test_trace_buffer_is_invisible_to_the_sim_heap() {
    FaultConfig cfg = one_site(SiteId::malloc, rule_with(0.0, FaultKind::OutOfMemory));
    auto scenario = cosmos::Scenario::create(kSeed, std::move(cfg), 1, recording(1024));
    must(scenario.has_value());

    scenario->run([] {
        for (int block = 0; block < 100; ++block) {
            void* ptr = std::malloc(32);
            must(ptr != nullptr);
            std::free(ptr);
        }
    });
    scenario->quiesce();

    const auto* injector = scenario->simulator().injector_or_null();
    must(injector != nullptr);
    assert(injector->trace().size() == 100);
    assert(scenario->report().active_allocations == 0);
    assert(scenario->check_no_leaks("trace-buffer-is-not-the-applications"));

    // The same oracle on the same fixture, with one block genuinely held: it counts what the
    // workload keeps, so the zero above is a measurement rather than a blind counter.
    FaultConfig with_rule = one_site(SiteId::malloc, rule_with(0.0, FaultKind::OutOfMemory));
    auto leaking = cosmos::Scenario::create(kSeed, std::move(with_rule), 1, recording(1024));
    must(leaking.has_value());
    leaking->run([] {
        void* held = std::malloc(32);
        must(held != nullptr);
    });
    leaking->quiesce();
    assert(leaking->report().active_allocations == 1);
    assert(!leaking->check_no_leaks("trace-buffer-is-not-the-applications"));

    std::cout << "[PASS] test_trace_buffer_is_invisible_to_the_sim_heap" << std::endl;
}

} // namespace

int main() {
    test_trace_is_off_until_enabled();
    test_entries_carry_site_time_amount_and_status();
    test_mixed_decisions_are_recorded_in_seq_order();
    test_drew_and_lost_entries_outnumber_the_ledger();
    test_zero_draw_passes_are_recorded();
    test_gate_blocked_calls_are_not_recorded();
    test_same_seed_gives_an_identical_trace();
    test_trace_overflow_is_counted();
    test_a_million_decisions_are_all_recorded();
    test_recording_never_allocates_with_a_universe_current();
    test_an_enabled_trace_with_no_capacity_is_rejected();
    test_trace_buffer_is_invisible_to_the_sim_heap();
    std::cout << "All trace tests passed successfully!" << std::endl;
    return 0;
}
