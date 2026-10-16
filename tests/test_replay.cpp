#include "cosmos/scenario.hpp"
#include "cosmos/trace.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using cosmos::DecisionStatus;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultKind;
using cosmos::FaultRule;
using cosmos::FaultTrace;
using cosmos::SiteId;
using cosmos::TraceEntry;
using cosmos::TraceFilter;
using cosmos::TraceOptions;

namespace {

constexpr uint64_t kSeed = 0x5CA1AB1E;
constexpr int kIterations = 24;

using Simulator = cosmos::Simulator;

void must(bool ok) { assert(ok); }

TraceOptions recording(uint64_t capacity = 1024) {
    return TraceOptions{.enabled = true, .capacity = capacity};
}

// Two sites, and the loop changes shape when malloc fails: it switches to a realloc-only path for
// good. That is what makes withholding one fault a *permanent* site divergence rather than a
// one-entry hiccup — a single-site or self-realigning fixture would hide exactly the case the
// cursor's site check exists to catch.
struct Counts {
    int malloc_calls = 0;
    int calloc_calls = 0;
    int realloc_calls = 0;
    int oomed = 0;
    bool fallback = false;
};

// The workload both the source run and every replay use; only the iteration count varies.
void drive(int iterations, Counts& counts) {
    for (int i = 0; i < iterations; ++i) {
        if (!counts.fallback) {
            void* block = std::malloc(32);
            ++counts.malloc_calls;
            if (block == nullptr) {
                ++counts.oomed;
                counts.fallback = true;
                continue;
            }
            void* table = std::calloc(4, 8);
            ++counts.calloc_calls;
            std::free(table);
            std::free(block);
            continue;
        }
        void* block = std::realloc(nullptr, 32);
        ++counts.realloc_calls;
        std::free(block);
    }
}

int total_calls(const Counts& counts) {
    return counts.malloc_calls + counts.calloc_calls + counts.realloc_calls;
}

FaultConfig two_site_plan(double malloc_rate, double calloc_rate) {
    FaultConfig plan;
    plan.enable_class(FaultClass::Memory);
    must(plan.activate_site(SiteId::malloc));
    must(plan.activate_site(SiteId::calloc));
    must(plan.activate_site(SiteId::realloc));

    FaultRule malloc_rule;
    malloc_rule.rate = malloc_rate;
    must(malloc_rule.outcomes.add(FaultKind::OutOfMemory, 1.0));
    must(plan.set_rule(SiteId::malloc, std::move(malloc_rule)));

    FaultRule calloc_rule;
    calloc_rule.rate = calloc_rate;
    must(calloc_rule.outcomes.add(FaultKind::OutOfMemory, 1.0));
    must(plan.set_rule(SiteId::calloc, std::move(calloc_rule)));

    // The fallback path asks for nothing: a rule of rate 0 still counts every call as eligible, so
    // it is a decision the cursor has to step over without inventing anything.
    FaultRule no_rule;
    no_rule.rate = 0.0;
    must(plan.set_rule(SiteId::realloc, std::move(no_rule)));

    return plan;
}

std::string printed(const Simulator& sim) {
    std::ostringstream out;
    cosmos::print_ledger(out, *sim.injector_or_null());
    return out.str();
}

bool same_trace(const FaultTrace& left, const FaultTrace& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (!(left[index] == right[index])) return false;
    }
    return true;
}

// Where the source run first put a fault on this site, or 0 when it never did.
uint64_t first_fire_seq(const FaultTrace& trace, SiteId site) {
    for (const TraceEntry& entry : trace) {
        if (entry.site == site && entry.outcome != FaultKind::None) return entry.seq;
    }
    return 0;
}

// Draws the source run spent within a prefix of its decisions, which is what a replay must match.
uint64_t draws_in_prefix(const FaultTrace& trace, uint64_t count) {
    uint64_t draws = 0;
    for (uint64_t index = 0; index < count && index < trace.size(); ++index) {
        if (trace[index].drew) ++draws;
    }
    return draws;
}

// True when the two traces agree everywhere except that one entry's status.
bool differs_only_at_status(const FaultTrace& source, const FaultTrace& replay, uint64_t seq) {
    if (source.size() != replay.size()) return false;
    for (size_t index = 0; index < source.size(); ++index) {
        TraceEntry expected = source[index];
        if (expected.seq == seq) expected.status = DecisionStatus::Suppressed;
        if (!(expected == replay[index])) return false;
    }
    return true;
}

// A source run plus everything a replay has to reproduce: its trace, its ledger and its counters.
struct Source {
    FaultTrace trace;
    std::string ledger;
    Counts counts;
    uint64_t injections = 0;
};

Source record_source(const FaultConfig& plan, int iterations) {
    auto scenario = cosmos::Scenario::create(kSeed, plan, 1, recording());
    must(scenario.has_value());

    Source source;
    scenario->run([&] { drive(iterations, source.counts); });
    scenario->quiesce();

    const auto* injector = scenario->simulator().injector_or_null();
    must(injector != nullptr);
    source.trace = injector->trace();
    source.ledger = printed(scenario->simulator());
    source.injections = injector->injections(SiteId::malloc) +
                        injector->injections(SiteId::calloc) +
                        injector->injections(SiteId::realloc);
    return source;
}

// Runs one replay and hands back the world it produced.
struct Replay {
    Counts counts;
    FaultTrace trace;
    std::string ledger;
    uint64_t decisions = 0;
    uint64_t draws = 0;
    uint64_t consumed = 0;
    uint64_t withheld = 0;
    uint64_t injections = 0;
};

Replay run_replay(const FaultConfig& plan, const FaultTrace& trace, TraceFilter filter,
                  int iterations) {
    auto scenario = cosmos::Scenario::replay(trace, std::move(filter), plan);
    must(scenario.has_value());

    Replay replay;
    scenario->run([&] { drive(iterations, replay.counts); });
    scenario->quiesce();

    const auto* injector = scenario->simulator().injector_or_null();
    must(injector != nullptr);
    replay.trace = injector->trace();
    replay.ledger = printed(scenario->simulator());
    replay.decisions = injector->decisions();
    replay.draws = injector->draws();
    replay.consumed = injector->replay_consumed();
    replay.withheld = injector->replay_withheld();
    replay.injections = injector->injections(SiteId::malloc) +
                        injector->injections(SiteId::calloc) +
                        injector->injections(SiteId::realloc);
    return replay;
}

void test_a_unmodified_trace_replays_the_run_identically() {
    const FaultConfig plan = two_site_plan(0.25, 0.25);
    const Source source = record_source(plan, kIterations);

    // Without a fire there is nothing to replay, and every comparison below would pass vacuously.
    must(source.injections > 0);
    must(source.counts.oomed > 0);
    assert(source.trace.size() == static_cast<size_t>(total_calls(source.counts)));

    const Replay replay = run_replay(plan, source.trace, TraceFilter::honour_all(), kIterations);

    assert(same_trace(source.trace, replay.trace));
    assert(replay.ledger == source.ledger);
    assert(replay.counts.malloc_calls == source.counts.malloc_calls);
    assert(replay.counts.calloc_calls == source.counts.calloc_calls);
    assert(replay.counts.realloc_calls == source.counts.realloc_calls);
    assert(replay.counts.oomed == source.counts.oomed);
    assert(replay.counts.fallback == source.counts.fallback);
    assert(replay.injections == source.injections);
    assert(replay.consumed == source.trace.size());
    assert(replay.withheld == 0);
    assert(replay.draws == draws_in_prefix(source.trace, source.trace.size()));

    std::cout << "[PASS] test_a_unmodified_trace_replays_the_run_identically" << std::endl;
}

void test_two_replays_of_one_trace_are_identical() {
    const FaultConfig plan = two_site_plan(0.25, 0.25);
    const Source source = record_source(plan, kIterations);
    must(source.injections > 0);

    const Replay left = run_replay(plan, source.trace, TraceFilter::honour_all(), kIterations);
    const Replay right = run_replay(plan, source.trace, TraceFilter::honour_all(), kIterations);

    assert(same_trace(left.trace, right.trace));
    assert(left.ledger == right.ledger);
    assert(left.draws == right.draws);
    assert(left.decisions == right.decisions);

    std::cout << "[PASS] test_two_replays_of_one_trace_are_identical" << std::endl;
}

// §11.2: a withheld entry still consumes its draw, or every later honoured entry would read the
// stream from the wrong place. Nothing downstream can see that except the draw count.
void test_a_withheld_fire_still_consumes_its_draw() {
    const FaultConfig plan = two_site_plan(0.25, 0.25);
    const Source source = record_source(plan, kIterations);

    // A calloc fire: nothing in the workload reads that result, so withholding it leaves control
    // flow alone and every later entry still has to line up.
    const uint64_t seq = first_fire_seq(source.trace, SiteId::calloc);
    must(seq != 0);

    const Replay replay = run_replay(plan, source.trace, TraceFilter::withhold({seq}), kIterations);

    assert(replay.withheld == 1);
    assert(replay.consumed == source.trace.size());
    assert(replay.decisions == source.trace.size());
    // The withheld entry's draw was spent anyway: one draw per drew=yes entry, withheld or not.
    assert(replay.draws == draws_in_prefix(source.trace, source.trace.size()));
    // Every other entry is honoured identically; only that row's status moved.
    assert(differs_only_at_status(source.trace, replay.trace, seq));
    assert(replay.injections == source.injections - 1);
    assert(replay.counts.calloc_calls == source.counts.calloc_calls);
    // §11.1: the ledger shows what the application was spared, not only what it endured.
    assert(replay.ledger != source.ledger);
    assert(replay.ledger.find("SUPPRESSED") != std::string::npos);

    std::cout << "[PASS] test_a_withheld_fire_still_consumes_its_draw" << std::endl;
}

// Rule 12. Suppressing the OOM makes the replay run the calloc the source skipped, so from there
// the live site and the cursor disagree and never line up again: everything must pass through.
void test_divergence_invents_no_faults() {
    const FaultConfig plan = two_site_plan(0.25, 0.25);
    const Source source = record_source(plan, kIterations);

    const uint64_t seq = first_fire_seq(source.trace, SiteId::malloc);
    must(seq != 0);

    const Replay replay = run_replay(plan, source.trace, TraceFilter::withhold({seq}), kIterations);

    assert(replay.withheld == 1);
    // Divergence happened, and the replay kept running past it rather than stopping.
    assert(replay.consumed < source.trace.size());
    assert(replay.trace.size() > replay.consumed);
    // The stream stopped where the divergence did: no draw after the last consumed entry.
    assert(replay.draws == draws_in_prefix(source.trace, replay.consumed));
    // Nothing was invented: no outcome, and no draw, in any post-divergence decision.
    for (size_t index = replay.consumed; index < replay.trace.size(); ++index) {
        assert(replay.trace[index].outcome == FaultKind::None);
        assert(!replay.trace[index].drew);
        assert(replay.trace[index].status == DecisionStatus::Fired);
    }
    // The replay kept allocating while the source had switched away: real divergence, and no fire.
    assert(!replay.counts.fallback);
    assert(source.counts.fallback);
    assert(replay.injections == source.injections - 1);

    std::cout << "[PASS] test_divergence_invents_no_faults" << std::endl;
}

void test_an_exhausted_trace_passes_through() {
    const FaultConfig plan = two_site_plan(0.25, 0.25);
    const Source source = record_source(plan, kIterations);
    must(source.injections > 0);

    const Replay replay =
        run_replay(plan, source.trace, TraceFilter::honour_all(), kIterations * 2);

    assert(replay.consumed == source.trace.size());
    assert(replay.trace.size() > source.trace.size());
    // Everything past the end of the trace behaved like a run with no rule armed at all.
    for (size_t index = source.trace.size(); index < replay.trace.size(); ++index) {
        assert(replay.trace[index].outcome == FaultKind::None);
        assert(!replay.trace[index].drew);
    }
    assert(replay.draws == draws_in_prefix(source.trace, source.trace.size()));
    assert(replay.injections == source.injections);

    std::cout << "[PASS] test_an_exhausted_trace_passes_through" << std::endl;
}

void test_a_truncated_trace_is_refused() {
    const FaultConfig plan = two_site_plan(1.0, 0.0);
    auto scenario = cosmos::Scenario::create(kSeed, plan, 1, recording(4));
    must(scenario.has_value());

    Counts counts;
    scenario->run([&] { drive(kIterations, counts); });
    scenario->quiesce();

    const auto* injector = scenario->simulator().injector_or_null();
    must(injector != nullptr);
    const FaultTrace truncated = injector->trace();
    must(truncated.dropped() > 0);

    auto replay = cosmos::Scenario::replay(truncated, TraceFilter::honour_all(), plan);
    must(!replay.has_value());
    assert(replay.error().error == cosmos::ConfigError::TruncatedTrace);

    std::cout << "[PASS] test_a_truncated_trace_is_refused" << std::endl;
}

void test_only_faults_keeps_exactly_the_named_fires() {
    // Its own workload: every armed site fires on every call, and a third site is armed at rate 0
    // so the trace holds decisions that inject nothing at all.
    const FaultConfig plan = two_site_plan(1.0, 1.0);
    auto scenario = cosmos::Scenario::create(kSeed, plan, 1, recording());
    must(scenario.has_value());
    scenario->run([] {
        for (int i = 0; i < 4; ++i) {
            std::free(std::malloc(16));
            std::free(std::calloc(1, 8));
            std::free(std::realloc(nullptr, 16));
        }
    });
    scenario->quiesce();

    const auto* injector = scenario->simulator().injector_or_null();
    must(injector != nullptr);
    const FaultTrace trace = injector->trace();

    uint64_t fires = 0;
    for (const TraceEntry& entry : trace) {
        if (entry.outcome != FaultKind::None) ++fires;
    }
    must(fires > 1);
    must(fires < trace.size());

    const uint64_t kept = trace[0].seq;
    must(trace[0].outcome != FaultKind::None);

    const TraceFilter filter = TraceFilter::only_faults(trace, {kept});
    assert(filter.withheld() == fires - 1);
    assert(!filter.withholds(kept));
    for (const TraceEntry& entry : trace) {
        if (entry.seq == kept) continue;
        // A kept fire is honoured, every other fire is withheld, and a decision that injected
        // nothing is never withheld — there is no fault in it to withhold.
        assert(filter.withholds(entry.seq) == (entry.outcome != FaultKind::None));
    }

    assert(TraceFilter::honour_all().withheld() == 0);

    std::cout << "[PASS] test_only_faults_keeps_exactly_the_named_fires" << std::endl;
}

} // namespace

int main() {
    test_a_unmodified_trace_replays_the_run_identically();
    test_two_replays_of_one_trace_are_identical();
    test_a_withheld_fire_still_consumes_its_draw();
    test_divergence_invents_no_faults();
    test_an_exhausted_trace_passes_through();
    test_a_truncated_trace_is_refused();
    test_only_faults_keeps_exactly_the_named_fires();
    std::cout << "All replay tests passed successfully!" << std::endl;
    return 0;
}
