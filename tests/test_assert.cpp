#include "cosmos/assert.hpp"
#include "cosmos/scenario.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr uint64_t kSeed = 4242;

void must(bool ok) { assert(ok); }

cosmos::Scenario make_with_plan(cosmos::FaultPlan plan, uint64_t seed = kSeed) {
    auto scenario = cosmos::Scenario::create(seed, std::move(plan));
    must(scenario.has_value());
    return std::move(*scenario);
}

cosmos::Scenario make(uint64_t seed = kSeed) { return make_with_plan(cosmos::FaultPlan{}, seed); }

const cosmos::CheckResult* find_check(const cosmos::ScenarioReport& report, const std::string& id) {
    for (const cosmos::CheckResult& result : report.checks) {
        if (result.id == id) return &result;
    }
    return nullptr;
}

const cosmos::CoverageNote* find_coverage(const cosmos::ScenarioReport& report,
                                          const std::string& id) {
    for (const cosmos::CoverageNote& note : report.coverage) {
        if (note.id == id) return &note;
    }
    return nullptr;
}

int count_coverage(const cosmos::ScenarioReport& report, const std::string& id) {
    int seen = 0;
    for (const cosmos::CoverageNote& note : report.coverage) {
        if (note.id == id) ++seen;
    }
    return seen;
}

// A passing invariant records nothing, so a workload whose only property held has still verified
// nothing: the vacuity rule outranks it.
void test_always_true_is_silent() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::always(true, "counter-monotonic"); });
    scenario.quiesce();

    const cosmos::ScenarioReport& report = scenario.report();
    must(find_check(report, "counter-monotonic") == nullptr);
    must(report.no_check_failed);
    must(report.vacuous());

    std::cout << "[PASS] test_always_true_is_silent" << std::endl;
}

void test_always_false_records_id_and_detail() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::always(false, "counter-monotonic", "counter went backwards"); });
    scenario.quiesce();

    const cosmos::ScenarioReport& report = scenario.report();
    const cosmos::CheckResult* result = find_check(report, "counter-monotonic");
    must(result != nullptr);
    must(!result->passed);
    must(result->detail == "counter went backwards");
    must(!report.no_check_failed);
    must(!report.passed());
    must(!report.vacuous());

    std::cout << "[PASS] test_always_false_records_id_and_detail" << std::endl;
}

// A bare COSMOS_CHECK names its own call site, and the detail survives to the report.
void test_cosmos_check_names_the_call_site() {
    cosmos::Scenario scenario = make();
    scenario.run([] { COSMOS_CHECK(1 + 1 == 3, "arithmetic"); });
    scenario.quiesce();

    const cosmos::CheckResult* result = find_check(scenario.report(), "arithmetic");
    must(result != nullptr);
    must(!result->passed);
    must(result->detail.find("test_assert.cpp:") != std::string::npos);

    std::cout << "[PASS] test_cosmos_check_names_the_call_site" << std::endl;
}

// A workload may call sometimes() in a loop; only the OR across calls is meaningful, so the id
// must appear once with the widest verdict.
void test_sometimes_coalesces_by_id() {
    cosmos::Scenario scenario = make();
    scenario.run([] {
        for (int i = 0; i < 5; ++i)
            cosmos::sometimes(i == 3, "retry-path");
    });
    scenario.quiesce();

    const cosmos::ScenarioReport& report = scenario.report();
    must(count_coverage(report, "retry-path") == 1);
    must(find_coverage(report, "retry-path")->hit);

    std::cout << "[PASS] test_sometimes_coalesces_by_id" << std::endl;
}

void test_sometimes_stays_visible_when_never_hit() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::sometimes(false, "disk-full-path"); });
    scenario.quiesce();

    const cosmos::CoverageNote* note = find_coverage(scenario.report(), "disk-full-path");
    must(note != nullptr);
    must(!note->hit);

    std::cout << "[PASS] test_sometimes_stays_visible_when_never_hit" << std::endl;
}

void test_reachable_marks_hit() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::reachable("warmup-complete"); });
    scenario.quiesce();

    const cosmos::CoverageNote* note = find_coverage(scenario.report(), "warmup-complete");
    must(note != nullptr);
    must(note->hit);

    std::cout << "[PASS] test_reachable_marks_hit" << std::endl;
}

// S0's sad path: the binding is per thread and restored on scope exit, so a second universe must
// neither see the first's violations nor inherit its coverage.
void test_assertions_do_not_leak_across_universes() {
    cosmos::Scenario first = make(kSeed);
    first.run([] {
        cosmos::always(false, "first-universe-only");
        cosmos::sometimes(false, "first-universe-path");
    });
    first.quiesce();

    cosmos::Scenario second = make(kSeed + 1);
    second.run([] { cosmos::always(true, "second-universe-only"); });
    second.quiesce();

    const cosmos::ScenarioReport& report = second.report();
    must(find_check(report, "first-universe-only") == nullptr);
    must(find_coverage(report, "first-universe-path") == nullptr);
    must(report.no_check_failed);
    must(report.coverage.empty());

    std::cout << "[PASS] test_assertions_do_not_leak_across_universes" << std::endl;
}

// check() runs the oracle under the same binding, so an always() inside an oracle is a violation
// rather than a crash.
void test_always_inside_oracle_is_recorded() {
    cosmos::Scenario scenario = make();
    scenario.run([] {});
    scenario.quiesce();
    scenario.check("replicas-agree", [] {
        cosmos::always(false, "replica-digest-match", "replica 2 differs");
        return true;
    });

    const cosmos::ScenarioReport& report = scenario.report();
    must(find_check(report, "replica-digest-match") != nullptr);
    must(!report.no_check_failed);

    std::cout << "[PASS] test_always_inside_oracle_is_recorded" << std::endl;
}

void test_violation_is_printed_by_report() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::always(false, "counter-monotonic", "counter went backwards"); });
    scenario.quiesce();

    std::ostringstream out;
    cosmos::print_report(out, scenario.report());
    const std::string text = out.str();
    must(text.find("counter-monotonic") != std::string::npos);
    must(text.find("counter went backwards") != std::string::npos);

    std::cout << "[PASS] test_violation_is_printed_by_report" << std::endl;
}

// F1: a violation with no universe bound has nowhere to record itself, so it must reach the
// out-of-scope channel -- counting it is what stops a release build from passing by ignoring it,
// and not aborting is what stops a build with no simulation running from dying.
void test_out_of_scope_assertions_are_counted_not_dropped() {
    const uint64_t count_before = cosmos::detail::out_of_scope_assertions().count;
    const bool first_ever = count_before == 0;

    cosmos::always(false, "outside-universe", "no scenario is bound");
    cosmos::sometimes(true, "outside-universe-path");
    cosmos::always(true, "outside-universe");

    const cosmos::detail::OutOfScopeAssertions& state = cosmos::detail::out_of_scope_assertions();
    must(state.count == count_before + 2);
    if (first_ever) {
        must(state.first_id == "outside-universe");
        must(state.first_detail == "no scenario is bound");
    }

    std::cout << "[PASS] test_out_of_scope_assertions_are_counted_not_dropped" << std::endl;
}

// F2: a passing check must not allocate. Short ids stay inside std::string's SSO, so any heap
// traffic here is the harness's own -- which the -static-libstdc++ twin is the only link that can
// see. The workload's own mallocs also keep this TU's link honest: without a wrapped-symbol
// reference, libcosmos is scanned before libstdc++.a asks for __wrap_free and the link fails.
void test_passing_check_does_not_allocate() {
    cosmos::FaultPlan plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    must(plan.activate_site(cosmos::SiteId::malloc));

    constexpr int kAllocations = 8;
    cosmos::Scenario scenario = make_with_plan(std::move(plan));
    scenario.run([] {
        for (int i = 0; i < kAllocations; ++i) {
            void* block = malloc(32);
            COSMOS_CHECK(block != nullptr, "alloc-ok");
            free(block);
        }
    });
    scenario.quiesce();

    // Exactly the workload's own allocations: a harness allocation would make this larger.
    must(scenario.report().eligible_calls[cosmos::site_slot(cosmos::SiteId::malloc)] ==
         static_cast<uint64_t>(kAllocations));

    std::cout << "[PASS] test_passing_check_does_not_allocate" << std::endl;
}

// F3: two live scenarios nested on one thread must each receive their own assertions, and popping
// the inner binding must restore the outer one rather than clearing it.
void test_nested_scenarios_route_and_restore() {
    cosmos::Scenario outer = make(kSeed);
    cosmos::Scenario inner = make(kSeed + 1);

    outer.run([&] {
        cosmos::always(false, "outer-violation");
        inner.run([] { cosmos::always(false, "inner-violation"); });
        cosmos::always(false, "outer-after-inner");
    });
    outer.quiesce();
    inner.quiesce();

    const cosmos::ScenarioReport& outer_report = outer.report();
    const cosmos::ScenarioReport& inner_report = inner.report();
    must(find_check(outer_report, "outer-violation") != nullptr);
    must(find_check(outer_report, "outer-after-inner") != nullptr);
    must(find_check(outer_report, "inner-violation") == nullptr);
    must(find_check(inner_report, "inner-violation") != nullptr);
    must(find_check(inner_report, "outer-violation") == nullptr);
    must(find_check(inner_report, "outer-after-inner") == nullptr);

    std::cout << "[PASS] test_nested_scenarios_route_and_restore" << std::endl;
}

} // namespace

int main() {
    test_always_true_is_silent();
    test_always_false_records_id_and_detail();
    test_cosmos_check_names_the_call_site();
    test_sometimes_coalesces_by_id();
    test_sometimes_stays_visible_when_never_hit();
    test_reachable_marks_hit();
    test_assertions_do_not_leak_across_universes();
    test_always_inside_oracle_is_recorded();
    test_violation_is_printed_by_report();
    test_out_of_scope_assertions_are_counted_not_dropped();
    test_passing_check_does_not_allocate();
    test_nested_scenarios_route_and_restore();
    std::cout << "All assertion tests passed successfully!" << std::endl;
    return 0;
}
