#include "cosmos/cosmos.hpp"
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

int main() {
    test_slot_is_empty_by_default();
    test_slot_holds_and_releases_an_injector();
    test_empty_slot_leaves_malloc_tracking_intact();
    test_other_instantiations_are_invisible_to_wrappers();
    test_malloc_passes_through_after_current_is_cleared();
    test_tracked_pointer_freed_with_no_current_simulator();
    test_destructor_clears_current();
    std::cout << "All simulator tests passed successfully!" << std::endl;
    return 0;
}
