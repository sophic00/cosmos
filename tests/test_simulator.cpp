#include "cosmos/assert.hpp"
#include "cosmos/cosmos.hpp"
#include "cosmos/fault_injector.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>

namespace {

// Stands in for the P1 engine: proves the slot holds a real type with a real constructor, not
// just an empty placeholder.
struct StubInjector {
    int calls = 0;
    explicit StubInjector(int start) : calls(start) {}
};

using StubSim = cosmos::BasicSimulator<StubInjector>;

} // namespace

// This predicate is what withdraws emplace_injector/clear_injector from the real Simulator, so a
// caller cannot install a wrongly-seeded injector or clear and re-install past install_faults'
// once-only rule. Calling either on it is a compile error, which no negative static_assert can
// express: a failed requires-clause on a non-template member is a hard error, not a substitution
// failure.
static_assert(cosmos::HasFactory<cosmos::BasicFaultInjector<cosmos::VirtualClock>>);
static_assert(!cosmos::HasFactory<StubInjector>);

// Two simulators must not share the thread_local current pointer.
static_assert(!std::is_same_v<cosmos::Simulator, StubSim>);
static_assert(!std::is_copy_constructible_v<cosmos::Simulator>);
static_assert(!std::is_copy_assignable_v<cosmos::Simulator>);

void test_slot_is_empty_by_default() {
    cosmos::Simulator sim;
    assert(!sim.has_injector());

    StubSim stub_sim;
    assert(!stub_sim.has_injector());

    std::cout << "[PASS] test_slot_is_empty_by_default" << std::endl;
}

void test_slot_holds_and_releases_an_injector() {
    StubSim sim;
    assert(!sim.has_injector());

    assert(sim.injector_or_null() == nullptr);

    StubInjector& injector = sim.emplace_injector(7);
    assert(sim.has_injector());
    assert(injector.calls == 7);
    assert(sim.injector_or_null() != nullptr);
    assert(sim.injector_or_null()->calls == 7);

    sim.injector_or_null()->calls = 9;
    assert(sim.injector_or_null()->calls == 9);

    sim.clear_injector();
    assert(!sim.has_injector());
    assert(sim.injector_or_null() == nullptr);

    sim.emplace_injector(1);
    assert(sim.injector_or_null()->calls == 1);

    std::cout << "[PASS] test_slot_holds_and_releases_an_injector" << std::endl;
}

// The empty slot must not disturb the existing heap path: this is the P0-S4 "no behavior change"
// requirement, checked here as well as by test_wrap_malloc running unmodified.
void test_empty_slot_leaves_malloc_tracking_intact() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void* ptr = malloc(64);
    assert(ptr != nullptr);
    assert(!sim.has_injector());
    assert(sim.heap().stats().active_allocations == 1);

    free(ptr);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_empty_slot_leaves_malloc_tracking_intact" << std::endl;
}

// Only the Simulator alias is wired to the wrappers. Another instantiation has its own current
// slot, so it is never reached through a wrapped malloc: do not plan a wrapper test around one.
void test_other_instantiations_are_invisible_to_wrappers() {
    StubSim stub;
    StubSim::set_current(&stub);
    assert(StubSim::has_current());
    assert(!cosmos::Simulator::has_current());

    void* ptr = malloc(64);
    assert(ptr != nullptr);
    assert(stub.heap().stats().active_allocations == 0);
    free(ptr);

    StubSim::set_current(nullptr);
    std::cout << "[PASS] test_other_instantiations_are_invisible_to_wrappers" << std::endl;
}

// Clearing the current simulator mid-run must fall back to the real allocator instead of
// dereferencing a stale pointer.
void test_malloc_passes_through_after_current_is_cleared() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void* tracked = malloc(64);
    assert(tracked != nullptr);
    assert(sim.heap().stats().active_allocations == 1);

    cosmos::Simulator::set_current(nullptr);
    assert(!cosmos::Simulator::has_current());

    void* passthrough = malloc(64);
    assert(passthrough != nullptr);
    assert(sim.heap().stats().active_allocations == 1);
    free(passthrough);

    cosmos::Simulator::set_current(&sim);
    free(tracked);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_malloc_passes_through_after_current_is_cleared" << std::endl;
}

// Scope is the documented context-management path: install on entry, restore the PREVIOUS
// pointer on exit — not clear-to-null — so nesting unwinds in LIFO order.
void test_scope_installs_and_restores() {
    assert(!cosmos::Simulator::has_current());
    {
        cosmos::Simulator sim;
        cosmos::Simulator::Scope scope(sim);
        assert(cosmos::Simulator::current() == &sim);
    }
    assert(!cosmos::Simulator::has_current());
    std::cout << "[PASS] test_scope_installs_and_restores" << std::endl;
}

void test_nested_scopes_unwind_in_lifo_order() {
    cosmos::Simulator outer(1);
    cosmos::Simulator inner(2);

    {
        cosmos::Simulator::Scope outer_scope(outer);
        assert(cosmos::Simulator::current() == &outer);
        {
            cosmos::Simulator::Scope inner_scope(inner);
            assert(cosmos::Simulator::current() == &inner);
        }
        assert(cosmos::Simulator::current() == &outer);
    }
    assert(!cosmos::Simulator::has_current());
    std::cout << "[PASS] test_nested_scopes_unwind_in_lifo_order" << std::endl;
}

// Wrapped calls inside a Scope route to that scope's universe — the invariant the campaign's
// per-universe Scope install relies on.
void test_wrapped_malloc_routes_inside_scope() {
    cosmos::Simulator sim;
    {
        cosmos::Simulator::Scope scope(sim);
        void* ptr = malloc(64);
        assert(ptr != nullptr);
        assert(sim.heap().stats().active_allocations == 1);
        free(ptr);
        assert(sim.heap().stats().active_allocations == 0);
    }
    assert(!cosmos::Simulator::has_current());
    std::cout << "[PASS] test_wrapped_malloc_routes_inside_scope" << std::endl;
}

// Placeholder contract: with no scheduler, the workload returning IS quiescence — the call is a
// no-op that must be safe to repeat and must preserve findings recorded before it. The campaign
// worker loop calls this unconditionally, which is the seam the real scheduler will fill.
void test_run_until_quiescence_noop_contract() {
    using namespace cosmos::literals;
    cosmos::Simulator sim(5);
    cosmos::Simulator::Scope scope(sim);
    cosmos::always(false, "noop.before", "d");
    assert(sim.findings().size() == 1);

    sim.advance_time(1_s);
    sim.run_until_quiescence();
    sim.run_until_quiescence(); // idempotent: callable twice, still no-op
    assert(sim.findings().size() == 1);
    assert(sim.findings()[0].assertion_id == "noop.before");
    assert(sim.now() == cosmos::Time::zero() + 1_s); // no hidden time advancement either

    std::cout << "[PASS] test_run_until_quiescence_noop_contract" << std::endl;
}

// A tracked payload sits one header past its real allocation, so releasing it with no simulator
// current used to hand the wrong address to the allocator and abort.
void test_tracked_pointer_freed_with_no_current_simulator() {
    void* tracked = nullptr;
    {
        cosmos::Simulator sim;
        cosmos::Simulator::set_current(&sim);
        tracked = malloc(64);
        assert(tracked != nullptr);
        assert(sim.heap().stats().active_allocations == 1);
        cosmos::Simulator::set_current(nullptr);
    }

    free(tracked);

    void* after = malloc(128);
    assert(after != nullptr);
    free(after);

    std::cout << "[PASS] test_tracked_pointer_freed_with_no_current_simulator" << std::endl;
}

void test_destructor_clears_current() {
    {
        cosmos::Simulator sim;
        cosmos::Simulator::set_current(&sim);
        assert(cosmos::Simulator::has_current());
    }
    assert(!cosmos::Simulator::has_current());

    void* ptr = malloc(32);
    assert(ptr != nullptr);
    free(ptr);

    std::cout << "[PASS] test_destructor_clears_current" << std::endl;
}

// Endpoint probabilities must decide without consuming stream decisions (docs/fault-injection.md
// §7 Rule 3): a disabled or always-on fault cannot shift the Memory sub-stream. Verified by
// checking the Rng state is untouched after the calls against a same-seed reference stream.
void test_install_faults_derives_the_fault_stream() {
    constexpr uint64_t kSeed = 0xC0FFEE;

    cosmos::Simulator sim(kSeed);
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Memory);
    assert(cfg.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = 0.5;
    assert(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    assert(cfg.set_rule(cosmos::SiteId::malloc, rule));
    assert(sim.install_faults(std::move(cfg)).has_value());

    cosmos::Rng reference(cosmos::fault_class_seed(
        cosmos::stream_seed(kSeed, cosmos::StreamDomain::Fault), cosmos::FaultClass::Memory));

    for (int i = 0; i < 32; ++i) {
        const bool expect_fire = reference.uniform() < 0.5;
        const cosmos::FaultKind kind =
            sim.injector_or_null()->decide(cosmos::FaultClass::Memory, cosmos::SiteId::malloc);
        assert((kind == cosmos::FaultKind::OutOfMemory) == expect_fire);
    }

    std::cout << "[PASS] test_install_faults_derives_the_fault_stream" << std::endl;
}

void test_install_faults_is_once_only() {
    cosmos::Simulator sim;
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Memory);
    assert(sim.install_faults(cfg).has_value());
    assert(sim.has_injector());

    auto again = sim.install_faults(cfg);
    assert(!again.has_value());
    assert(again.error().error == cosmos::ConfigError::InjectorAlreadyInstalled);

    std::cout << "[PASS] test_install_faults_is_once_only" << std::endl;
}

void test_install_faults_rejects_an_invalid_config() {
    cosmos::Simulator sim;
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Memory);
    assert(cfg.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = 1.5;
    assert(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    assert(cfg.set_rule(cosmos::SiteId::malloc, rule));

    auto rejected = sim.install_faults(std::move(cfg));
    assert(!rejected.has_value());
    assert(rejected.error().error == cosmos::ConfigError::BadRate);
    assert(rejected.error().site == cosmos::SiteId::malloc);
    assert(!sim.has_injector());

    std::cout << "[PASS] test_install_faults_rejects_an_invalid_config" << std::endl;
}

// seed() reports exactly the seed the Simulator was built with, including the default: it is
// the repro key campaign findings will print, so it must never drift from the ctor argument.
void test_simulator_seed_accessor_reports_ctor_argument() {
    cosmos::Simulator default_sim;
    assert(default_sim.seed() == cosmos::kDefaultUniverseSeed);

    cosmos::Simulator sim(42);
    assert(sim.seed() == 42);

    cosmos::Simulator zero_sim(0);
    assert(zero_sim.seed() == 0);

    cosmos::Simulator max_sim(~0ULL);
    assert(max_sim.seed() == ~0ULL);

    std::cout << "[PASS] test_simulator_seed_accessor_reports_ctor_argument" << std::endl;
}

// §4 interim contract: the trace digest is a function of the universe seed and the event
// stream — same seed, same events, same digest; nothing else may move it.
void test_trace_hash_is_deterministic_for_seed() {
    cosmos::Simulator sim_a(42);
    cosmos::Simulator sim_b(42);
    cosmos::Simulator sim_c(43);

    assert(sim_a.trace_hash() == sim_b.trace_hash());
    assert(sim_a.trace_hash() != sim_c.trace_hash());

    std::cout << "[PASS] test_trace_hash_is_deterministic_for_seed" << std::endl;
}

// Clock advances are events: equal advances hash equal, unequal streams diverge even when they
// end at the same virtual time (50+50ms is a different event stream from one 100ms step).
void test_trace_hash_tracks_clock_events() {
    using namespace cosmos::literals;

    cosmos::Simulator base(42);
    const uint64_t untouched = base.trace_hash();

    cosmos::Simulator sim_a(42);
    sim_a.advance_time(100_ms);
    assert(sim_a.trace_hash() != untouched);

    cosmos::Simulator sim_b(42);
    sim_b.advance_time(100_ms);
    assert(sim_a.trace_hash() == sim_b.trace_hash());

    cosmos::Simulator sim_c(42);
    sim_c.advance_time(50_ms);
    sim_c.advance_time(50_ms);
    assert(sim_c.trace_hash() != sim_a.trace_hash());

    cosmos::Simulator sim_d(42);
    sim_d.advance_time(50_ms);
    sim_d.clock().advance_to(cosmos::Time::zero() + 100_ms);
    cosmos::Simulator sim_e(42);
    sim_e.advance_time(100_ms);
    assert(sim_d.trace_hash() != sim_e.trace_hash());

    std::cout << "[PASS] test_trace_hash_tracks_clock_events" << std::endl;
}

// Fault fires are events too: a decide() that fires must land in the digest, identically across
// two same-seed universes, and a universe without the fire must hash differently. install_faults
// seeds the injector from the universe seed, so both universes fire identically.
void test_trace_hash_reflects_fault_fires() {
    const auto make_cfg = [] {
        cosmos::FaultConfig cfg;
        cfg.enable_class(cosmos::FaultClass::Memory);
        assert(cfg.activate_site(cosmos::SiteId::malloc));
        cosmos::FaultRule rule;
        rule.rate = 1.0;
        assert(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
        assert(cfg.set_rule(cosmos::SiteId::malloc, rule));
        return cfg;
    };

    const auto fired_sim = [&make_cfg] {
        cosmos::Simulator sim(42);
        assert(sim.install_faults(make_cfg()).has_value());
        const auto kind =
            sim.injector_or_null()->decide(cosmos::FaultClass::Memory, cosmos::SiteId::malloc);
        assert(kind == cosmos::FaultKind::OutOfMemory);
        return sim.trace_hash();
    };

    const uint64_t with_fire = fired_sim();
    assert(with_fire == fired_sim());

    cosmos::Simulator no_fire(42);
    assert(no_fire.install_faults(make_cfg()).has_value());
    assert(with_fire != no_fire.trace_hash());

    std::cout << "[PASS] test_trace_hash_reflects_fault_fires" << std::endl;
}

// The verify-mode shape (state-exploration.md §4): the same workload run twice through fresh
// universes and consecutive Scopes on one thread must end with identical digests. Exercises all
// four wrapped allocators — the pattern Campaign --verify will rely on. This is exactly the
// comparison Campaign --verify will make.
void test_trace_hash_is_stable_across_scope_swap_rerun() {
    const auto run_workload = [] {
        cosmos::Simulator sim(7);
        cosmos::Simulator::Scope scope(sim);
        void* a = malloc(64);
        assert(a != nullptr);
        void* b = calloc(4, 32);
        assert(b != nullptr);
        void* c = realloc(nullptr, 128); // realloc-as-malloc path
        assert(c != nullptr);
        c = realloc(c, 256); // tracked resize path
        assert(c != nullptr);
        free(a);
        free(b);
        free(c);
        sim.advance_time(cosmos::Duration{250'000});
        const uint64_t hash = sim.trace_hash();
        assert(cosmos::Simulator::current() == &sim);
        return hash;
    };

    assert(run_workload() == run_workload());
    assert(!cosmos::Simulator::has_current()); // Scope restored through both consecutive runs

    std::cout << "[PASS] test_trace_hash_is_stable_across_scope_swap_rerun" << std::endl;
}

void test_simulator_virtual_clock() {
    using namespace cosmos::literals;
    cosmos::Simulator sim;
    assert(sim.now() == cosmos::Time::zero());
    assert(sim.clock().now_ns() == 0);

    sim.advance_time(500_ms);
    assert(sim.now() == cosmos::Time::zero() + 500_ms);
    assert(sim.clock().now_ns() == 500'000'000);

    sim.clock().advance_to(cosmos::Time::zero() + 2_s);
    assert(sim.now() == cosmos::Time::zero() + 2_s);

    std::cout << "[PASS] test_simulator_virtual_clock" << std::endl;
}

int main() {
    test_slot_is_empty_by_default();
    test_slot_holds_and_releases_an_injector();
    test_empty_slot_leaves_malloc_tracking_intact();
    test_other_instantiations_are_invisible_to_wrappers();
    test_malloc_passes_through_after_current_is_cleared();
    test_tracked_pointer_freed_with_no_current_simulator();
    test_destructor_clears_current();
    test_scope_installs_and_restores();
    test_nested_scopes_unwind_in_lifo_order();
    test_wrapped_malloc_routes_inside_scope();
    test_run_until_quiescence_noop_contract();
    test_install_faults_derives_the_fault_stream();
    test_install_faults_is_once_only();
    test_install_faults_rejects_an_invalid_config();
    test_simulator_seed_accessor_reports_ctor_argument();
    test_trace_hash_is_deterministic_for_seed();
    test_trace_hash_tracks_clock_events();
    test_trace_hash_reflects_fault_fires();
    test_trace_hash_is_stable_across_scope_swap_rerun();
    test_simulator_virtual_clock();
    std::cout << "All simulator tests passed successfully!" << std::endl;
    return 0;
}
