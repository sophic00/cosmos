#include "cosmos/cosmos.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace {

// Stands in for the P1 engine: proves the slot holds a real type with a real constructor, not
// just an empty placeholder.
struct StubInjector {
    int calls = 0;
    explicit StubInjector(int start) : calls(start) {}
};

using StubSim = cosmos::BasicSimulator<StubInjector>;

} // namespace

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
void test_oom_decision_endpoint_rates_do_not_draw() {
    cosmos::FaultProfile disabled;
    disabled.oom_rate = 0.0;
    cosmos::FaultProfile always;
    always.oom_rate = 1.0;

    cosmos::Rng reference(99);

    cosmos::Rng probe(99);
    for (int i = 0; i < 8; ++i) {
        assert(!disabled.should_inject_oom(probe));
    }
    assert(probe.next() == reference.next());

    for (int i = 0; i < 8; ++i) {
        assert(always.should_inject_oom(probe));
    }
    assert(probe.next() == reference.next());

    std::cout << "[PASS] test_oom_decision_endpoint_rates_do_not_draw" << std::endl;
}

// Intermediate rates draw one decision per call: identical seeds produce identical patterns.
void test_oom_decision_is_deterministic_for_fixed_seed() {
    cosmos::FaultProfile fp;
    fp.oom_rate = 0.5;

    const auto draw_pattern = [fp](uint64_t seed) {
        cosmos::Rng rng(cosmos::fault_class_seed(seed, cosmos::FaultClass::Memory));
        std::array<bool, 64> pattern{};
        for (bool& decision : pattern) {
            decision = fp.should_inject_oom(rng);
        }
        return pattern;
    };

    assert(draw_pattern(42) == draw_pattern(42));

    std::cout << "[PASS] test_oom_decision_is_deterministic_for_fixed_seed" << std::endl;
}

// Each Simulator owns its Memory fault sub-stream derived from its seed: same seed => same
// decisions, different seed => different decisions (a 64-draw collision is vanishingly unlikely).
void test_simulator_seed_drives_fault_stream() {
    cosmos::FaultProfile fp;
    fp.oom_rate = 0.5;

    cosmos::Simulator sim_a(42);
    sim_a.set_faults(fp);
    cosmos::Simulator sim_b(42);
    sim_b.set_faults(fp);
    cosmos::Simulator sim_c(43);
    sim_c.set_faults(fp);

    std::array<bool, 64> pattern_a{};
    std::array<bool, 64> pattern_b{};
    std::array<bool, 64> pattern_c{};
    for (size_t i = 0; i < pattern_a.size(); ++i) {
        pattern_a[i] = sim_a.faults().should_inject_oom(sim_a.fault_rng());
        pattern_b[i] = sim_b.faults().should_inject_oom(sim_b.fault_rng());
        pattern_c[i] = sim_c.faults().should_inject_oom(sim_c.fault_rng());
    }

    assert(pattern_a == pattern_b);
    assert(pattern_a != pattern_c);

    std::cout << "[PASS] test_simulator_seed_drives_fault_stream" << std::endl;
}

int main() {
    test_slot_is_empty_by_default();
    test_slot_holds_and_releases_an_injector();
    test_empty_slot_leaves_malloc_tracking_intact();
    test_other_instantiations_are_invisible_to_wrappers();
    test_malloc_passes_through_after_current_is_cleared();
    test_tracked_pointer_freed_with_no_current_simulator();
    test_destructor_clears_current();
    test_oom_decision_endpoint_rates_do_not_draw();
    test_oom_decision_is_deterministic_for_fixed_seed();
    test_simulator_seed_drives_fault_stream();
    std::cout << "All simulator tests passed successfully!" << std::endl;
    return 0;
}
