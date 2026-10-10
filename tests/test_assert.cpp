#include "cosmos/assert.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::string_view_literals;

namespace {

constexpr const char* kIdA = "kv.read-your-writes";
constexpr const char* kIdB = "kv.crash-exercised";
constexpr const char* kIdC = "kv.recovery-hit";

// Shared campaign-wide singleton; each test resets it before use so never_hit output is exact.
cosmos::SometimesRegistry& the_registry() { return cosmos::sometimes_registry(); }

} // namespace

// A violation under an active universe records exactly one Failure carrying that universe's
// seed, the id, and the detail. The happy path records nothing.
void test_always_records_violation_with_universe_seed() {
    cosmos::Simulator sim(4242);
    cosmos::Simulator::Scope scope(sim);

    cosmos::always(true, kIdA, "unused");
    assert(sim.findings().empty());

    cosmos::always(false, kIdA, "detail-1");
    cosmos::always(false, kIdA, "detail-2");
    assert(sim.findings().size() == 2); // every violation is a finding; grouping is campaign-level
    assert(sim.findings()[0].seed == 4242);
    assert(sim.findings()[0].assertion_id == kIdA);
    assert(sim.findings()[0].detail == "detail-1");
    assert(sim.findings()[1].detail == "detail-2");

    std::cout << "[PASS] test_always_records_violation_with_universe_seed" << std::endl;
}

// COSMOS_ALWAYS captures file:line at compile time, with no per-call formatting cost.
void test_cosmos_always_macro_captures_file_line() {
    cosmos::Simulator sim(1);
    cosmos::Simulator::Scope scope(sim);

    COSMOS_ALWAYS(false, kIdA);
    assert(sim.findings().size() == 1);
    const std::string& detail = sim.findings()[0].detail;
    assert(detail.find("test_assert.cpp:") != std::string::npos);
    assert(detail.find(":") != std::string::npos);

    COSMOS_ALWAYS(true, kIdA);
    assert(sim.findings().size() == 1);

    std::cout << "[PASS] test_cosmos_always_macro_captures_file_line" << std::endl;
}

// Registration on every call is what makes never_hit work: an id registered but never satisfied
// anywhere is reported; a satisfied id is not; both appear as registered.
void test_sometimes_never_hit_lifecycle() {
    the_registry().reset();

    cosmos::sometimes(false, kIdA); // registered, not hit
    cosmos::sometimes(true, kIdB);  // registered, hit
    cosmos::sometimes(false, kIdA); // registered again

    const auto never_hit = the_registry().never_hit_ids();
    assert(never_hit.size() == 1);
    assert(never_hit[0] == kIdA);

    const auto registered = the_registry().registered_ids();
    assert(registered.size() == 2);
    assert(std::find(registered.begin(), registered.end(), kIdA) != registered.end());
    assert(std::find(registered.begin(), registered.end(), kIdB) != registered.end());

    assert(the_registry().registrations(kIdA) == 2);
    assert(the_registry().hits(kIdA) == 0);
    assert(the_registry().hits(kIdB) == 1);
    assert(the_registry().registrations("absent.id") == 0);

    std::cout << "[PASS] test_sometimes_never_hit_lifecycle" << std::endl;
}

// The report contract: registered-but-never-satisfied ids come back sorted.
void test_sometimes_never_hit_ids_are_sorted() {
    the_registry().reset();

    cosmos::sometimes(false, "zeta.id");
    cosmos::sometimes(false, "alpha.id");
    cosmos::sometimes(false, "mid.id");

    const auto never_hit = the_registry().never_hit_ids();
    assert(never_hit.size() == 3);
    assert(std::is_sorted(never_hit.begin(), never_hit.end()));
    assert(never_hit[0] == "alpha.id");

    std::cout << "[PASS] test_sometimes_never_hit_ids_are_sorted" << std::endl;
}

// The registry is campaign-wide state shared by worker threads; concurrent register/mark from
// several threads must lose nothing.
void test_sometimes_registry_thread_safety() {
    the_registry().reset();

    constexpr int kThreads = 4;
    constexpr int kCalls = 1'000;

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < kCalls; ++i) {
                cosmos::sometimes(t % 2 == 0, t % 2 == 0 ? kIdA : kIdB);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    // 2000 registrations of each id; even-thread hits A, odd-thread hits B.
    assert(the_registry().registrations(kIdA) == 2 * kCalls);
    assert(the_registry().registrations(kIdB) == 2 * kCalls);
    assert(the_registry().hits(kIdA) == 2 * kCalls);
    assert(the_registry().hits(kIdB) == 0);

    const auto never_hit = the_registry().never_hit_ids();
    assert(never_hit.size() == 1);
    assert(never_hit[0] == kIdB);

    std::cout << "[PASS] test_sometimes_registry_thread_safety" << std::endl;
}

int main() {
    test_always_records_violation_with_universe_seed();
    test_cosmos_always_macro_captures_file_line();
    test_sometimes_never_hit_lifecycle();
    test_sometimes_never_hit_ids_are_sorted();
    test_sometimes_registry_thread_safety();
    std::cout << "All assert tests passed successfully!" << std::endl;
    return 0;
}
