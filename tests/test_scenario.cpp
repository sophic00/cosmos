#include "cosmos/scenario.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>

using namespace cosmos::literals;

namespace {

constexpr uint64_t kSeed = 8421;

void must(bool ok) { assert(ok); }

cosmos::FaultPlan malloc_plan(double rate) {
    cosmos::FaultPlan plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    must(plan.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = rate;
    must(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    must(plan.set_rule(cosmos::SiteId::malloc, std::move(rule)));
    return plan;
}

cosmos::Scenario make(cosmos::FaultPlan plan, uint64_t seed = kSeed) {
    auto scenario = cosmos::Scenario::create(seed, std::move(plan));
    must(scenario.has_value());
    return std::move(*scenario);
}

const cosmos::CheckResult* find_check(const cosmos::ScenarioReport& report, const std::string& id) {
    for (const cosmos::CheckResult& result : report.checks) {
        if (result.id == id) return &result;
    }
    return nullptr;
}

// One scenario can accumulate several lifecycle entries, so the detail is what identifies which.
bool has_lifecycle(const cosmos::ScenarioReport& report, const std::string& needle) {
    for (const cosmos::CheckResult& result : report.checks) {
        if (result.id == cosmos::kLifecycleId && result.detail.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The §17 teaser, end to end. The workload allocates into a stack array so that nothing but its
// own mallocs is eligible: fail_on_call counts the universe's allocations, harness included.
void test_minimal_example_fails_the_nth_allocation() {
    constexpr uint64_t kFailOn = 443;
    constexpr int kAllocations = 445;
    static void* blocks[kAllocations];
    static int failed_index = -1;

    const cosmos::ScenarioReport report = cosmos::run(
        {.seed = kSeed, .oom = {.fail_on_call = kFailOn}},
        [] {
            for (int i = 0; i < kAllocations; ++i) {
                blocks[i] = malloc(16);
                if (blocks[i] == nullptr && failed_index < 0) failed_index = i;
            }
        },
        [] { return failed_index == static_cast<int>(kFailOn) - 1; });

    assert(report.passed());
    assert(failed_index == static_cast<int>(kFailOn) - 1);

    // The universe died with cosmos::run, so every surviving block outlived its heap: the frees
    // below go through the orphan path, and the registry entry must be gone afterwards.
    using cosmos::detail::AllocRegistry;
    using cosmos::detail::OwnerKind;
    for (int i = 0; i < kAllocations; ++i) {
        if (i == failed_index) continue;
        assert(blocks[i] != nullptr);
        assert(AllocRegistry::instance().ownership_of(blocks[i]).kind == OwnerKind::Orphaned);
        free(blocks[i]);
        assert(AllocRegistry::instance().ownership_of(blocks[i]).kind == OwnerKind::None);
    }
    assert(report.eligible_calls[cosmos::site_slot(cosmos::SiteId::malloc)] == kAllocations);
    assert(report.injections[cosmos::site_slot(cosmos::SiteId::malloc)] == 1);

    std::cout << "[PASS] test_minimal_example_fails_the_nth_allocation" << std::endl;
}

void test_check_and_note_covered_record_id_and_detail() {
    cosmos::Scenario scenario = make(malloc_plan(0.0));
    scenario.run([] {});
    scenario.quiesce();

    assert(scenario.check("stays-consistent", [] { return true; }, "key=user:42"));
    assert(!scenario.check("no-data-loss", [] { return false; }, "node=n1"));
    scenario.note_covered("oom-path-exercised", true);
    scenario.note_covered("crash-path-exercised", false);

    const cosmos::ScenarioReport& report = scenario.report();
    assert(!report.passed());
    assert(report.checks.size() == 2);
    assert(report.checks[0].id == "stays-consistent");
    assert(report.checks[0].detail == "key=user:42");
    assert(report.checks[0].passed);
    assert(report.checks[1].id == "no-data-loss");
    assert(report.checks[1].detail == "node=n1");
    assert(!report.checks[1].passed);
    assert(report.coverage.size() == 2);
    assert(report.coverage[0].id == "oom-path-exercised" && report.coverage[0].hit);
    assert(report.coverage[1].id == "crash-path-exercised" && !report.coverage[1].hit);

    std::cout << "[PASS] test_check_and_note_covered_record_id_and_detail" << std::endl;
}

void test_failing_check_reports_seed_and_ledger() {
    cosmos::Scenario scenario = make(malloc_plan(1.0));
    scenario.run([] {
        void* p = malloc(32);
        assert(p == nullptr);
    });
    scenario.quiesce();
    assert(!scenario.check("no-committed-data-loss", [] { return false; }, "node=n1 key=user:42"));

    std::ostringstream out;
    cosmos::print_report(out, scenario);
    const std::string text = out.str();

    assert(text.find("FAILED: \"no-committed-data-loss\"") != std::string::npos);
    assert(text.find("seed    = 8421") != std::string::npos);
    assert(text.find("detail  = \"node=n1 key=user:42\"") != std::string::npos);
    assert(text.find("t       = 0ms") != std::string::npos);
    assert(text.find("Fault ledger:") != std::string::npos);
    assert(text.find("site=malloc") != std::string::npos);
    assert(text.find("OutOfMemory") != std::string::npos);
    assert(text.find("malloc eligible=1 fired=1") != std::string::npos);

    std::cout << "[PASS] test_failing_check_reports_seed_and_ledger" << std::endl;
}

// The oracle must not run out of order: the world has not settled, so its answer means nothing.
void test_check_before_quiesce_is_a_lifecycle_failure() {
    static bool oracle_ran = false;
    cosmos::Scenario scenario = make(malloc_plan(0.0));
    scenario.run([] {});

    assert(!scenario.check("premature", [] {
        oracle_ran = true;
        return true;
    }));
    assert(!oracle_ran);

    const cosmos::ScenarioReport& report = scenario.report();
    assert(!report.passed());
    assert(find_check(report, "premature") == nullptr);
    const cosmos::CheckResult* lifecycle = find_check(report, cosmos::kLifecycleId);
    assert(lifecycle != nullptr && !lifecycle->passed);
    assert(lifecycle->detail.find("before quiesce()") != std::string::npos);

    std::cout << "[PASS] test_check_before_quiesce_is_a_lifecycle_failure" << std::endl;
}

// note_covered records a fact the caller already holds, so unlike check it observes nothing and
// needs no settled world; the natural pattern notes a flag straight after the workload.
void test_note_covered_is_legal_before_quiesce() {
    static bool oom_path_hit = false;
    cosmos::Scenario scenario = make(malloc_plan(1.0));
    scenario.run([] { oom_path_hit = (malloc(16) == nullptr); });
    scenario.note_covered("oom-path-exercised", oom_path_hit);
    scenario.quiesce();

    assert(scenario.report().coverage.size() == 1);
    assert(scenario.report().coverage[0].hit);
    assert(find_check(scenario.report(), cosmos::kLifecycleId) == nullptr);
    assert(scenario.report().no_check_failed);
    assert(!scenario.passed()); // no check was registered, so nothing was verified

    std::cout << "[PASS] test_note_covered_is_legal_before_quiesce" << std::endl;
}

// A scenario whose run() was skipped must not be able to report PASSED.
void test_quiesce_without_run_is_a_lifecycle_failure() {
    cosmos::Scenario scenario = make(malloc_plan(0.0));
    scenario.quiesce();

    assert(scenario.check("oracle", [] { return true; }));
    assert(!scenario.report().passed());
    const cosmos::CheckResult* lifecycle = find_check(scenario.report(), cosmos::kLifecycleId);
    assert(lifecycle != nullptr);
    assert(lifecycle->detail.find("without run()") != std::string::npos);

    std::cout << "[PASS] test_quiesce_without_run_is_a_lifecycle_failure" << std::endl;
}

void test_run_sugar_names_the_check() {
    const cosmos::ScenarioReport report = cosmos::run(
        {.seed = kSeed, .check_id = "no-committed-data-loss"}, [] {}, [] { return false; });

    assert(!report.passed());
    assert(find_check(report, "no-committed-data-loss") != nullptr);
    assert(find_check(report, "oracle") == nullptr);

    std::cout << "[PASS] test_run_sugar_names_the_check" << std::endl;
}

// The oracle judges this universe, so a wrapped call inside it must read the universe's clock.
void test_oracle_observes_the_universe() {
    static int64_t seen_ns = -1;
    cosmos::Scenario scenario = make(malloc_plan(0.0));
    scenario.run([] {});
    scenario.simulator().advance_time(1250_ms);
    scenario.quiesce();

    assert(scenario.check("reads-virtual-time", [] {
        if (!cosmos::Simulator::has_current()) return false;
        struct timespec ts{};
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return false;
        seen_ns = static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
        return true;
    }));
    assert(seen_ns == 1'250'000'000);
    assert(!cosmos::Simulator::has_current());

    std::cout << "[PASS] test_oracle_observes_the_universe" << std::endl;
}

// The workload must not run at all: under the permanent quiet window it would be a fault-free dry
// run wearing the shape of a chaos phase.
void test_run_after_quiesce_is_refused() {
    static bool workload_ran = false;
    static bool faulted = false;

    // The sharp case: quiesce() first, so the ran_ guard cannot be what stops this. Without the
    // quiesced_ guard the workload executes under the permanent quiet window and sees no faults.
    cosmos::Scenario fresh = make(malloc_plan(1.0));
    fresh.quiesce();
    fresh.run([] {
        workload_ran = true;
        faulted = malloc(16) == nullptr;
    });
    assert(!workload_ran);
    assert(!faulted);
    assert(has_lifecycle(fresh.report(), "after quiesce()"));

    cosmos::Scenario scenario = make(malloc_plan(1.0));
    scenario.run([] {});
    scenario.quiesce();
    scenario.run([] { workload_ran = true; });

    assert(!workload_ran);
    assert(has_lifecycle(scenario.report(), "after quiesce()"));
    assert(!scenario.passed());

    std::cout << "[PASS] test_run_after_quiesce_is_refused" << std::endl;
}

// The universe dies with the sugar, so the ledger has to be rendered before it does or no caller
// could ever print it.
void test_sugar_carries_the_ledger_on_failure() {
    const cosmos::ScenarioReport failed = cosmos::run(
        {.seed = kSeed, .oom = {.rate = 1.0}}, [] { assert(malloc(16) == nullptr); },
        [] { return false; });

    assert(!failed.passed());
    assert(!failed.ledger_dump.empty());
    assert(failed.ledger_dump.find("FIRED") != std::string::npos);
    assert(failed.ledger_dump.find("site=malloc") != std::string::npos);

    std::ostringstream out;
    cosmos::print_report(out, failed);
    assert(out.str().find("Fault ledger:") != std::string::npos);

    const cosmos::ScenarioReport passed = cosmos::run({.seed = kSeed}, [] {}, [] { return true; });
    assert(passed.passed());
    assert(passed.ledger_dump.empty());

    std::cout << "[PASS] test_sugar_carries_the_ledger_on_failure" << std::endl;
}

// The other half of the vacuous pass: no checks means nothing was verified.
void test_zero_check_scenario_is_vacuous_not_passed() {
    cosmos::Scenario scenario = make(malloc_plan(0.0));
    scenario.run([] {});
    scenario.quiesce();

    assert(scenario.report().no_check_failed);
    assert(scenario.report().vacuous());
    assert(!scenario.passed());

    std::ostringstream out;
    cosmos::print_report(out, scenario);
    assert(out.str().find("VACUOUS: no checks registered") != std::string::npos);
    assert(out.str().find("PASSED") == std::string::npos);
    // Nothing failed, so there is nothing for a ledger to explain.
    assert(out.str().find("Fault ledger:") == std::string::npos);

    std::cout << "[PASS] test_zero_check_scenario_is_vacuous_not_passed" << std::endl;
}

// A spec that asks for no faults must not arm the site: an activated malloc counts every
// allocation as eligible, making a no-fault report's counters read like a fault run's.
void test_default_spec_arms_nothing() {
    const cosmos::ScenarioReport report = cosmos::run(
        {.seed = kSeed},
        [] {
            for (int i = 0; i < 10; ++i) {
                void* p = malloc(16);
                free(p);
            }
        },
        [] { return true; });

    assert(report.passed());
    assert(report.eligible_calls[cosmos::site_slot(cosmos::SiteId::malloc)] == 0);
    assert(report.injections[cosmos::site_slot(cosmos::SiteId::malloc)] == 0);

    const cosmos::ScenarioReport armed = cosmos::run(
        {.seed = kSeed,
         .oom =
             {
                 .rate = 0.0,
             }},
        [] {
            void* p = malloc(16);
            free(p);
        },
        [] { return true; });
    assert(armed.eligible_calls[cosmos::site_slot(cosmos::SiteId::malloc)] == 0);

    // "Asked for nothing" must not swallow "asked for something impossible": a negative rate is a
    // misconfiguration and has to keep reaching validate().
    const cosmos::ScenarioReport bad =
        cosmos::run({.seed = kSeed, .oom = {.rate = -0.5}}, [] {}, [] { return true; });
    assert(!bad.passed());
    const cosmos::CheckResult* rejection = find_check(bad, cosmos::kConfigId);
    assert(rejection != nullptr);
    assert(rejection->detail == std::string("BadRate (site=malloc)"));

    std::cout << "[PASS] test_default_spec_arms_nothing" << std::endl;
}

void test_run_twice_is_a_lifecycle_failure() {
    static int runs = 0;
    cosmos::Scenario scenario = make(malloc_plan(0.0));
    scenario.run([] { ++runs; });
    scenario.run([] { ++runs; });

    assert(runs == 1);
    assert(!scenario.report().passed());
    const cosmos::CheckResult* lifecycle = find_check(scenario.report(), cosmos::kLifecycleId);
    assert(lifecycle != nullptr);
    assert(lifecycle->detail.find("more than once") != std::string::npos);

    std::cout << "[PASS] test_run_twice_is_a_lifecycle_failure" << std::endl;
}

void test_quiesce_is_idempotent() {
    cosmos::Scenario scenario = make(malloc_plan(1.0));
    scenario.run([] { assert(malloc(16) == nullptr); });
    scenario.quiesce();
    const uint64_t after_first =
        scenario.report().eligible_calls[cosmos::site_slot(cosmos::SiteId::malloc)];
    scenario.quiesce();
    scenario.quiesce();

    assert(scenario.report().eligible_calls[cosmos::site_slot(cosmos::SiteId::malloc)] ==
           after_first);
    assert(!scenario.passed()); // zero checks so far: nothing verified
    assert(scenario.check("still-usable", [] { return true; }));
    assert(scenario.passed());

    std::cout << "[PASS] test_quiesce_is_idempotent" << std::endl;
}

// §9.3: after quiesce no fault may fire. Nothing else pins this, because outside run() the
// universe is not current and its allocations never reach the injector at all.
void test_no_fault_fires_after_quiesce() {
    cosmos::Scenario scenario = make(malloc_plan(1.0));
    scenario.run([] { assert(malloc(16) == nullptr); });
    scenario.quiesce();

    const size_t slot = cosmos::site_slot(cosmos::SiteId::malloc);
    assert(scenario.report().injections[slot] == 1);

    cosmos::Simulator::set_current(&scenario.simulator());
    void* p = malloc(16);
    assert(p != nullptr);
    free(p);
    cosmos::Simulator::set_current(nullptr);

    assert(scenario.simulator().injector_or_null()->eligible_calls(cosmos::SiteId::malloc) == 1);
    assert(scenario.simulator().injector_or_null()->injections(cosmos::SiteId::malloc) == 1);

    std::cout << "[PASS] test_no_fault_fires_after_quiesce" << std::endl;
}

void test_counters_are_snapshotted_at_quiesce() {
    cosmos::Scenario scenario = make(malloc_plan(0.0));
    scenario.run([] {
        for (int i = 0; i < 7; ++i) {
            void* p = malloc(16);
            free(p);
        }
    });
    scenario.quiesce();

    const size_t slot = cosmos::site_slot(cosmos::SiteId::malloc);
    assert(scenario.report().eligible_calls[slot] == 7);

    // The oracle now runs with the universe current, so its raw allocation is tracked by the heap
    // but the quiet window keeps it out of the eligible count the snapshot froze.
    const size_t tracked_before = scenario.simulator().heap().stats().total_allocation_count;
    scenario.check("allocates", [] {
        void* p = malloc(64);
        const bool ok = p != nullptr;
        free(p);
        return ok;
    });
    assert(scenario.simulator().heap().stats().total_allocation_count == tracked_before + 1);
    assert(scenario.report().eligible_calls[slot] == 7);
    assert(scenario.simulator().injector_or_null()->eligible_calls(cosmos::SiteId::malloc) == 7);

    std::cout << "[PASS] test_counters_are_snapshotted_at_quiesce" << std::endl;
}

void test_two_scenarios_do_not_contaminate_each_other() {
    cosmos::Scenario first = make(malloc_plan(1.0), 111);
    cosmos::Scenario second = make(malloc_plan(0.0), 222);

    first.run([] { assert(malloc(16) == nullptr); });
    second.run([] {
        void* p = malloc(16);
        assert(p != nullptr);
        free(p);
    });
    first.quiesce();
    second.quiesce();

    const size_t slot = cosmos::site_slot(cosmos::SiteId::malloc);
    assert(first.report().injections[slot] == 1);
    assert(second.report().injections[slot] == 0);
    assert(first.report().seed == 111 && second.report().seed == 222);
    assert(first.simulator().injector_or_null()->ledger().size() == 1);
    assert(second.simulator().injector_or_null()->ledger().size() == 0);
    assert(&first.simulator() != &second.simulator());

    std::cout << "[PASS] test_two_scenarios_do_not_contaminate_each_other" << std::endl;
}

void test_run_restores_the_previous_universe() {
    assert(!cosmos::Simulator::has_current());
    cosmos::Simulator outer(999);
    cosmos::Simulator::set_current(&outer);

    cosmos::Scenario scenario = make(malloc_plan(0.0));
    scenario.run([] { assert(cosmos::Simulator::current() != nullptr); });
    assert(cosmos::Simulator::current() == &outer);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_run_restores_the_previous_universe" << std::endl;
}

void test_invalid_plan_is_rejected_at_create() {
    cosmos::FaultPlan plan = malloc_plan(1.5);
    auto rejected = cosmos::Scenario::create(kSeed, std::move(plan));
    assert(!rejected.has_value());
    assert(rejected.error().error == cosmos::ConfigError::BadRate);

    // fire_on_eligible_call must exceed skip_first, so a zero-th allocation can never fire.
    const cosmos::ScenarioReport report =
        cosmos::run({.seed = kSeed, .oom = {.fail_on_call = 0}}, [] {}, [] { return true; });
    assert(!report.passed());
    const cosmos::CheckResult* rejection = find_check(report, cosmos::kConfigId);
    assert(rejection != nullptr);
    assert(rejection->detail == std::string("TriggerLeSkipFirst (site=malloc)"));

    std::cout << "[PASS] test_invalid_plan_is_rejected_at_create" << std::endl;
}

void test_same_seed_twice_reports_identically() {
    const auto once = [](uint64_t seed) {
        cosmos::Scenario scenario = make(malloc_plan(0.4), seed);
        scenario.run([] {
            for (int i = 0; i < 40; ++i) {
                void* p = malloc(16);
                if (p) free(p);
            }
        });
        scenario.quiesce();
        scenario.check("oracle", [] { return false; }, "detail");
        std::ostringstream out;
        cosmos::print_report(out, scenario);
        return out.str();
    };

    const std::string first = once(kSeed);
    assert(first == once(kSeed));
    assert(first != once(kSeed + 1));
    assert(first.find("FIRED") != std::string::npos);

    std::cout << "[PASS] test_same_seed_twice_reports_identically" << std::endl;
}

// Scenario::on drives a universe the caller owns, which is what lets the campaign runner build one
// Simulator per universe without handing its ownership to the harness.
void test_scenario_on_borrows_an_externally_owned_simulator() {
    cosmos::Simulator sim(kSeed);

    {
        auto scenario = cosmos::Scenario::on(sim, cosmos::FaultPlan{});
        must(scenario.has_value());
        must(scenario->report().seed == kSeed);

        scenario->run([] {});
        scenario->quiesce();
        must(scenario->check("oracle", [] { return true; }));
        must(scenario->report().passed());
    }

    // The scenario is gone; the universe it borrowed is not, and is still the caller's to drive.
    cosmos::Simulator::set_current(&sim);
    void* block = malloc(16);
    must(block != nullptr);
    free(block);
    cosmos::Simulator::set_current(nullptr);
    must(sim.heap().active_count() == 0);

    std::cout << "[PASS] test_scenario_on_borrows_an_externally_owned_simulator" << std::endl;
}

// A second install on one universe would fork its streams and counters, so the borrow is refused.
void test_scenario_on_refuses_a_second_install() {
    cosmos::Simulator sim(kSeed);
    must(cosmos::Scenario::on(sim, cosmos::FaultPlan{}).has_value());

    auto second = cosmos::Scenario::on(sim, cosmos::FaultPlan{});
    must(!second.has_value());
    must(second.error().error == cosmos::ConfigError::InjectorAlreadyInstalled);

    std::cout << "[PASS] test_scenario_on_refuses_a_second_install" << std::endl;
}

} // namespace

int main() {
    test_minimal_example_fails_the_nth_allocation();
    test_check_and_note_covered_record_id_and_detail();
    test_failing_check_reports_seed_and_ledger();
    test_check_before_quiesce_is_a_lifecycle_failure();
    test_note_covered_is_legal_before_quiesce();
    test_quiesce_without_run_is_a_lifecycle_failure();
    test_run_sugar_names_the_check();
    test_oracle_observes_the_universe();
    test_run_after_quiesce_is_refused();
    test_sugar_carries_the_ledger_on_failure();
    test_zero_check_scenario_is_vacuous_not_passed();
    test_default_spec_arms_nothing();
    test_run_twice_is_a_lifecycle_failure();
    test_quiesce_is_idempotent();
    test_no_fault_fires_after_quiesce();
    test_counters_are_snapshotted_at_quiesce();
    test_two_scenarios_do_not_contaminate_each_other();
    test_run_restores_the_previous_universe();
    test_invalid_plan_is_rejected_at_create();
    test_same_seed_twice_reports_identically();
    test_scenario_on_borrows_an_externally_owned_simulator();
    test_scenario_on_refuses_a_second_install();
    std::cout << "All scenario tests passed successfully!" << std::endl;
    return 0;
}
