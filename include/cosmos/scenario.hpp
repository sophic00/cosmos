#pragma once

#include "cosmos/fault_injector.hpp"
#include "cosmos/faults.hpp"
#include "cosmos/ledger_print.hpp"
#include "cosmos/simulator.hpp"
#include <concepts>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cosmos {

using FaultPlan = FaultConfig;

template <typename F>
concept Workload = std::invocable<F>;

template <typename F>
concept Oracle = std::invocable<F> && std::convertible_to<std::invoke_result_t<F>, bool>;

// Reserved prefix: ids beginning with "cosmos." are the harness's own, never an application's.
inline constexpr const char* kLifecycleId = "cosmos.lifecycle";
inline constexpr const char* kConfigId = "cosmos.config";

struct CheckResult {
    std::string id;
    std::string detail;
    bool passed = false;
    Time at{};
};

struct CoverageNote {
    std::string id;
    bool hit = false;
};

struct ScenarioReport {
    uint64_t seed = 0;
    bool no_check_failed = true;
    std::vector<CheckResult> checks{};
    std::vector<CoverageNote> coverage{};
    // Snapshotted at quiesce because §11.4's vacuous-coverage guard reads counters, not entries,
    // and the campaign must not have to reach back into a finished universe's injector.
    SiteCounterMap eligible_calls{};
    SiteCounterMap injections{};
    // Snapshotted rather than read live: an oracle reading the count inside check()'s CurrentGuard
    // would include its own allocations wherever operator new reaches the wrappers.
    size_t active_allocations = 0;
    // Rendered before the universe dies, because the sugar returns the report by value and the
    // ledger would otherwise be unreachable. Empty on a pass, and for a Scenario used directly.
    std::string ledger_dump{};

    bool vacuous() const { return checks.empty(); }

    // A scenario that registered no check has verified nothing, so it cannot have passed.
    bool passed() const { return no_check_failed && !vacuous(); }
};

// The ledger explains a failure, so both dump sites gate on this rather than on !passed(): a
// vacuous run is also not-passed, and has nothing to explain.
inline bool failed(const ScenarioReport& report) { return !report.no_check_failed; }

// Lifecycle: create -> run (exactly once) -> quiesce (idempotent) -> at least one check.
// note_covered is free. Any deviation records a kLifecycleId failure instead of asserting, so a
// release build cannot pass by ignoring one.
class Scenario {
  public:
    [[nodiscard]] static std::expected<Scenario, ConfigProblem>
    create(uint64_t seed, FaultPlan plan, uint32_t node_count = 1) {
        auto sim = std::make_unique<Simulator>(seed);
        if (auto installed = sim->install_faults(std::move(plan), node_count); !installed) {
            return std::unexpected(installed.error());
        }
        return Scenario(seed, std::move(sim));
    }

    // A scenario over a universe the caller owns and outlives: the campaign runner builds one
    // Simulator per universe and must not hand its ownership to the harness.
    [[nodiscard]] static std::expected<Scenario, ConfigProblem> on(Simulator& sim, FaultPlan plan,
                                                                   uint32_t node_count = 1) {
        if (auto installed = sim.install_faults(std::move(plan), node_count); !installed) {
            return std::unexpected(installed.error());
        }
        return Scenario(sim);
    }

    template <Workload F> void run(F&& workload) {
        // Not merely out of order: the permanent quiet window would make this a fault-free dry run
        // wearing the shape of a chaos phase.
        if (quiesced_) {
            record_lifecycle("run() called after quiesce()");
            return;
        }
        if (ran_) {
            record_lifecycle("run() called more than once");
            return;
        }
        ran_ = true;
        CurrentGuard guard(*this);
        workload();
    }

    // §9.3's faults-off half, as a permanent quiet window, plus the fiber
    // drain: remaining tasks run to quiescence with faults OFF. Load-bearing
    // whenever the universe is made current again after the run — an oracle
    // reading persisted state does exactly that.
    void quiesce() {
        if (quiesced_) return;
        quiesced_ = true;
        // A scenario that never ran its workload would otherwise report PASSED: the harness must
        // not be capable of the vacuous pass §11.4 exists to catch.
        // Snapshot first in this path only: record_lifecycle allocates, and if this universe
        // happens to be current the complaint would land in the count it is about to describe
        // (observable under -static-libstdc++, where operator new reaches the wrappers). The
        // early return below keeps that snapshot, so harness bookkeeping never rewrites it.
        if (!ran_) {
            report_.active_allocations = sim_->heap().active_count();
            record_lifecycle("quiesce() called without run()");
            auto* early_injector = sim_->injector_or_null();
            if (early_injector != nullptr) early_injector->push_quiet();
            {
                CurrentGuard guard(*this);
                sim_->scheduler().run_until_quiescence();
            }
            return;
        }
        auto* injector = sim_->injector_or_null();
        if (injector != nullptr) injector->push_quiet();
        bool saw_deadlock = false;
        {
            CurrentGuard guard(*this);
            sim_->scheduler().run_until_quiescence();
            saw_deadlock = sim_->scheduler().deadlocked();
            // Snapshot under guard but drain itself uses __real_malloc only, so
            // no tracked allocation can land between the drain and this read.
            report_.active_allocations = sim_->heap().active_count();
            if (injector != nullptr) {
                for (size_t slot = 0; slot < kSiteCount; ++slot) {
                    const SiteId site = site_at_slot(slot);
                    report_.eligible_calls[slot] = injector->eligible_calls(site);
                    report_.injections[slot] = injector->injections(site);
                }
            }
        }
        // Outside the guard so the detail string cannot become a tracked block
        // the snapshot above just claimed to describe.
        if (saw_deadlock) record_lifecycle("deadlock detected during quiesce drain");
    }

    // The oracle is deliberately not evaluated out of order: the world has not settled, so its
    // answer would be meaningless rather than merely early.
    template <Oracle F> bool check(std::string id, F&& oracle, std::string detail = "") {
        if (!quiesced_) {
            record_lifecycle("check(\"" + id + "\") called before quiesce()");
            return false;
        }
        // The oracle judges this universe, so wrapped calls inside it must answer with the
        // universe's state and not the host's. Safe under the permanent quiet window: the quiet
        // gate precedes the eligible-call increment, so the oracle can neither fire a fault nor
        // move a counter.
        bool ok = false;
        Time at{};
        {
            CurrentGuard guard(*this);
            ok = static_cast<bool>(oracle());
            at = sim_->now();
        }
        // Outside the guard: the harness's own bookkeeping must not become a block this universe
        // owns, or it contaminates the accounting it is about to report — and P2-S3's leak check.
        report_.checks.push_back(CheckResult{std::move(id), std::move(detail), ok, at});
        if (!ok) report_.no_check_failed = false;
        return ok;
    }

    // Opt-in, not automatic at universe end: a workload that deliberately holds state past quiesce
    // — a cache is the ordinary case — is not leaking, and would false-positive on a blanket check.
    // Runs through check(), so it inherits the after-quiesce rule and reads the snapshot, never the
    // live count: one oracle's allocations must not change a later leak verdict.
    bool check_no_leaks(std::string id) {
        const size_t live = report_.active_allocations;
        return check(
            std::move(id), [live] { return live == 0; },
            "active_allocations=" + std::to_string(live));
    }

    // Legal at any point, unlike check(): this records a fact the caller already holds rather than
    // observing the world, so ordering it after quiesce would buy nothing and surprise the natural
    // "set a flag in the workload, note it straight after run()" pattern.
    void note_covered(std::string id, bool hit) {
        report_.coverage.push_back(CoverageNote{std::move(id), hit});
    }

    // The scenario a wrapped assertion on this thread reports to; null when no universe is active.
    static Scenario* current() { return current_; }

    // Mid-run invariant violations land here: unlike check(), this needs no preceding quiesce, so
    // an `always` inside the workload is recorded the moment it fails.
    void record_violation(std::string id, std::string detail = "") {
        report_.checks.push_back(CheckResult{std::move(id), std::move(detail), false, sim_->now()});
        report_.no_check_failed = false;
    }

    // Coalesced by id: a workload may call this in a loop, and only the OR across calls matters.
    void record_sometimes(std::string id, bool hit) {
        for (CoverageNote& note : report_.coverage) {
            if (note.id == id) {
                note.hit = note.hit || hit;
                return;
            }
        }
        report_.coverage.push_back(CoverageNote{std::move(id), hit});
    }

    bool passed() const { return report_.passed(); }

    const ScenarioReport& report() const { return report_; }
    Simulator& simulator() { return *sim_; }
    const Simulator& simulator() const { return *sim_; }

  private:
    // Restores both previous bindings rather than clearing them, so a nested or sequential
    // scenario cannot strand an outer one.
    struct CurrentGuard {
        explicit CurrentGuard(Scenario& scenario)
            : previous_universe_(Simulator::current()), previous_scenario_(Scenario::current()) {
            Simulator::set_current(scenario.sim_);
            Scenario::current_ = &scenario;
        }
        ~CurrentGuard() {
            Simulator::set_current(previous_universe_);
            Scenario::current_ = previous_scenario_;
        }

        CurrentGuard(const CurrentGuard&) = delete;
        CurrentGuard& operator=(const CurrentGuard&) = delete;

      private:
        Simulator* previous_universe_;
        Scenario* previous_scenario_;
    };

    Scenario(uint64_t seed, std::unique_ptr<Simulator> sim)
        : owned_(std::move(sim)), sim_(owned_.get()) {
        report_.seed = seed;
    }

    explicit Scenario(Simulator& sim) : sim_(&sim) { report_.seed = sim.seed(); }

    void record_lifecycle(std::string detail) {
        report_.checks.push_back(CheckResult{kLifecycleId, std::move(detail), false, sim_->now()});
        report_.no_check_failed = false;
    }

    // Empty when the universe is the caller's (Scenario::on); sim_ is non-owning either way.
    std::unique_ptr<Simulator> owned_;
    Simulator* sim_ = nullptr;
    ScenarioReport report_{};
    bool ran_ = false;
    bool quiesced_ = false;
    // thread_local on purpose, like the universe slot: a scenario belongs to the thread driving it.
    inline static thread_local Scenario* current_{nullptr};
};

// §17.4's shape. The ledger is dumped only on failure, because that is the one time a human reads
// it, and only through the injector, which owns the per-site counters the dump prints alongside.
inline void print_report(std::ostream& out, const ScenarioReport& report) {
    std::ostringstream body;
    if (report.vacuous()) {
        body << "VACUOUS: no checks registered  seed = " << report.seed << "\n";
    } else if (report.passed()) {
        body << "PASSED  seed = " << report.seed << "\n";
    }
    bool seed_printed = false;
    for (const CheckResult& result : report.checks) {
        if (result.passed) continue;
        body << "FAILED: \"" << result.id << "\"\n";
        if (!seed_printed) {
            body << "  seed    = " << report.seed << "\n";
            seed_printed = true;
        }
        if (!result.detail.empty()) body << "  detail  = \"" << result.detail << "\"\n";
        body << "  t       = " << detail::format_ms(result.at) << "\n\n";
    }
    for (const CoverageNote& note : report.coverage) {
        if (!note.hit) body << "NOT COVERED: \"" << note.id << "\"\n";
    }
    out << body.str();
    if (!report.ledger_dump.empty()) out << report.ledger_dump;
}

inline void print_report(std::ostream& out, const Scenario& scenario) {
    print_report(out, scenario.report());
    const auto* injector = scenario.simulator().injector_or_null();
    if (failed(scenario.report()) && injector != nullptr) print_ledger(out, *injector);
}

struct OomSpec {
    std::optional<uint64_t> fail_on_call{};
    double rate = 0.0;
};

struct RunSpec {
    uint64_t seed = kDefaultUniverseSeed;
    OomSpec oom{};
    uint32_t node_count = 1;
    std::string check_id = "oracle";
};

// fail_on_call is the K-th *eligible* call at SiteId::malloc (§10), counting every allocation that
// reaches the wrapped symbol. Which ones do is linkage-dependent: under the default shared
// libstdc++ operator new does not, so std:: containers are invisible; under -static-libstdc++ it
// does (see test_scenario_static). An exact count therefore needs the raw allocator, as the 443
// test does.
inline FaultPlan oom_plan(const OomSpec& oom) {
    FaultPlan plan;
    // A spec that asks for nothing arms nothing: an activated site counts every allocation as
    // eligible, which would make a no-fault report's counters look like a fault run's.
    // == rather than <=: a negative rate is a misconfiguration and must still reach
    // validate(), not be folded into "asked for nothing". -0.0 == 0.0, so zero still exits.
    if (oom.rate == 0.0 && !oom.fail_on_call.has_value()) return plan;
    plan.enable_class(FaultClass::Memory);
    (void)plan.activate_site(SiteId::malloc);
    FaultRule rule;
    rule.rate = oom.rate;
    (void)rule.outcomes.add(FaultKind::OutOfMemory, 1.0);
    rule.fire_on_eligible_call = oom.fail_on_call;
    (void)plan.set_rule(SiteId::malloc, std::move(rule));
    return plan;
}

// The universe is destroyed when this returns, so any block the workload hands back to the caller
// outlives its heap and is released through the orphan path. Nothing can count those blocks
// afterwards, which is why a universe-end leak check can never fire for the sugar.
template <Workload W, Oracle O> ScenarioReport run(RunSpec spec, W&& workload, O&& oracle) {
    auto scenario = Scenario::create(spec.seed, oom_plan(spec.oom), spec.node_count);
    if (!scenario) {
        const ConfigProblem problem = scenario.error();
        std::string detail = name_of(problem.error);
        if (problem.site) detail += " (site=" + std::string(name_of(*problem.site)) + ")";
        ScenarioReport rejected;
        rejected.seed = spec.seed;
        rejected.no_check_failed = false;
        rejected.checks.push_back(CheckResult{kConfigId, std::move(detail), false, Time::zero()});
        return rejected;
    }

    scenario->run(std::forward<W>(workload));
    scenario->quiesce();
    scenario->check(std::move(spec.check_id), std::forward<O>(oracle));

    ScenarioReport report = scenario->report();
    if (failed(report)) {
        if (const auto* injector = scenario->simulator().injector_or_null()) {
            std::ostringstream dump;
            print_ledger(dump, *injector);
            report.ledger_dump = dump.str();
        }
    }
    return report;
}

} // namespace cosmos
