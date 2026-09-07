#pragma once

#include "cosmos/assert.hpp"
#include "cosmos/finding.hpp"
#include "cosmos/simulator.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cosmos {

// Controls the campaign run (state-exploration.md §4).
struct CampaignConfig {
    uint64_t trials = 1'000;
    uint64_t base_seed = 0;
    unsigned parallel = std::thread::hardware_concurrency();
    bool verify = false; // double-run each seed, compare trace hashes (§8 determinism gate)

    // Reporting cap: per assertion_id, the printed report lists at most this many distinct
    // seeds. The full list always remains in CampaignReport::findings.
    uint64_t max_seeds_per_finding = 10;

    // Incremental flushing (Crash Containment item 2): the merged report is written to
    // report_path (if non-empty) every flush_every completed runs, immediately after any run
    // that produced findings, and once more after the pool shuts down. A crash then loses at
    // most the current universe — never the campaign, and never an already-found finding.
    std::string report_path;
    uint64_t flush_every = 100;

    // On-disk per-worker seed markers (Crash Containment item 1): before running a universe
    // each worker writes worker-<id>.seed into marker_dir and deletes it on clean completion.
    // After a crash, the surviving files name the in-flight seeds — those seeds ARE the
    // findings; read them back with crash_attributed_seeds(). Empty disables marker files.
    std::string marker_dir;
};

// Aggregated result of the full campaign. failed_runs counts universes (one run can produce
// several findings); findings counts violations. Output is made order-independent before
// printing: findings sorted by seed, never_hit sorted by id — the same campaign produces
// byte-identical output regardless of worker interleaving.
struct CampaignReport {
    uint64_t runs = 0;        // universes executed
    uint64_t failed_runs = 0; // universes with >= 1 Failure (or a crash)
    std::vector<Failure> findings;
    std::vector<std::string> never_hit; // sometimes() ids registered but never satisfied

    // Ids registered with the SometimesRegistry but satisfied in no universe, sorted.
    void compute_never_hit() { never_hit = sometimes_registry().never_hit_ids(); }

    void sort_for_stable_output() {
        std::sort(findings.begin(), findings.end(), [](const Failure& a, const Failure& b) {
            if (a.seed != b.seed) return a.seed < b.seed;
            if (a.assertion_id != b.assertion_id) return a.assertion_id < b.assertion_id;
            return a.detail < b.detail;
        });
        std::sort(never_hit.begin(), never_hit.end());
    }

    // Findings grouped by assertion_id, each group's seed list capped at
    // CampaignConfig::max_seeds_per_finding. One bug hit by 4,000 of 10,000 seeds is still one
    // bug; this is the grouping the printed report uses.
    std::map<std::string, std::vector<uint64_t>> grouped_findings(uint64_t cap) const {
        std::map<std::string, std::vector<uint64_t>> groups;
        for (const Failure& f : findings) {
            std::vector<uint64_t>& seeds = groups[f.assertion_id];
            if (seeds.size() < cap) {
                seeds.push_back(f.seed);
            }
        }
        return groups;
    }

    // Byte-stable text rendering (findings already sorted; groups keyed by sorted map order).
    std::string to_string(uint64_t max_seeds_per_finding) const {
        std::string out;
        const auto append = [&out](std::string_view text) { out.append(text); };
        for (const auto& [id, seeds] : grouped_findings(max_seeds_per_finding)) {
            append("FINDING: ");
            out.append(id);
            append(" (seeds:");
            for (uint64_t seed : seeds) {
                append(" --seed ");
                out.append(std::to_string(seed));
            }
            append(seeds.size() >= max_seeds_per_finding ? " ...)" : ")");
            append("\n");
        }
        for (const std::string& id : never_hit) {
            append("NEVER HIT: ");
            out.append(id);
            append("\n");
        }
        return out;
    }
};

// The campaign-wide context (state-exploration.md §4): the sometimes() registry lives here, so
// every worker thread reports into one shared state. Delegates to the singleton the
// SometimesRegistry owns; per-worker findings shards merge into the report under run()'s mutex.
class Campaign {
  public:
    static SometimesRegistry& current() { return sometimes_registry(); }
};

// Crash Containment item 1 (§4): a per-worker marker naming the seed the worker is currently
// running. If the process dies, the marker identifies the crashing seed — that seed IS the
// finding. The in-memory slot is the live copy; the on-disk twin (below) is what survives the
// process. Signal handling and fork-per-seed subprocess isolation are deferred until Phase 2's
// Process fault class makes crashing universes reproducible enough to exercise them.
class WorkerSeedMarker {
  public:
    void publish(uint64_t seed) {
        seed_.store(seed, std::memory_order_release);
        active_.store(true, std::memory_order_release);
    }
    void clear() { active_.store(false, std::memory_order_release); }
    bool active() const { return active_.load(std::memory_order_acquire); }
    uint64_t seed() const { return seed_.load(std::memory_order_acquire); }

  private:
    std::atomic<uint64_t> seed_{0};
    std::atomic<bool> active_{false};
};

namespace detail {

// The on-disk twin of WorkerSeedMarker: worker-<id>.seed holds the seed of the universe the
// worker is currently running. Survives the process, which is the point. All I/O is
// best-effort — a failed marker write must never abort the campaign.
inline std::filesystem::path seed_marker_path(const std::string& dir, unsigned worker_id) {
    return std::filesystem::path(dir) / ("worker-" + std::to_string(worker_id) + ".seed");
}

inline void write_seed_marker(const std::string& dir, unsigned worker_id, uint64_t seed) {
    if (dir.empty()) return;
    std::FILE* file = std::fopen(seed_marker_path(dir, worker_id).c_str(), "w");
    if (file == nullptr) return;
    std::fprintf(file, "%llu\n", static_cast<unsigned long long>(seed));
    std::fclose(file);
}

inline void remove_seed_marker(const std::string& dir, unsigned worker_id) {
    if (dir.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(seed_marker_path(dir, worker_id), ignored);
}

// Crash attribution: every marker file left in dir names a seed that was in flight when the
// process died. Those seeds ARE the findings. Sorted for stable output; files are left in
// place — the caller decides what to do with them.
inline std::vector<uint64_t> crash_attributed_seeds(const std::string& dir) {
    std::vector<uint64_t> seeds;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, error)) {
        std::FILE* file = std::fopen(entry.path().c_str(), "r");
        if (file == nullptr) continue;
        unsigned long long seed = 0;
        if (std::fscanf(file, "%llu", &seed) == 1) {
            seeds.push_back(seed);
        }
        std::fclose(file);
    }
    std::sort(seeds.begin(), seeds.end());
    return seeds;
}

// One worker's shard: findings accumulate locally; the merge under a single mutex happens per
// universe (and the flush cadence rides the same lock). record_finding is per-universe state,
// so this is where per-run results become campaign state.
struct WorkerShard {
    std::vector<Failure> findings;
    bool failed_run = false;
};

// Crash Containment item 2: write the merged report snapshot to disk. Best-effort — a failed
// flush must not abort the campaign; the in-memory report remains authoritative.
inline void flush_report_to_disk(const std::string& path, const CampaignReport& report,
                                 uint64_t max_seeds_per_finding) {
    if (path.empty()) return;
    if (FILE* file = std::fopen(path.c_str(), "w")) {
        const std::string text = report.to_string(max_seeds_per_finding);
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
}

} // namespace detail

// The campaign loop (state-exploration.md §4). Builds the seed list once (base_seed,
// base_seed+1, ... base_seed+trials), then starts a FIXED pool of config.parallel workers
// draining it through an atomic cursor — never one thread per trial. Each universe is pure
// function of its seed, so OS scheduling cannot change any universe's outcome; only the
// distribution across workers varies.
//
// Worker loop per seed:
//   1. publish the seed to this worker's marker, memory + disk (crash attribution; surviving
//      worker-<id>.seed files after a crash are readable via crash_attributed_seeds())
//   2. Scope scope(sim) — RAII context install, never manual set_current()
//   3. build_fn(sim, seed) — the user workload + assertions (the IDENTICAL build_fn that
//      run_single receives; factor it into a named function, or repro runs will diverge)
//   4. sim.run_until_quiescence()
//   5. verify mode: run sim2 through a second Scope on the same thread with the same seed and
//      compare live trace hashes; a mismatch is itself a finding ("determinism.violation"),
//      recorded and kept campaigning — never an abort
//   6. merge this worker's shard into the shared report (one mutex), flush on cadence
//
// never_hit is computed from the campaign-wide registry after the pool shuts down; the report
// is then sorted for stable output. The registry is NOT reset by run() — a campaign composes
// with whatever ids were registered before it.
//
// Deferred (Phase 2): signal handlers (SIGSEGV/SIGABRT → marker + report flush + re-raise) and
// fork-per-seed subprocess isolation. Both need the Process fault class to make crashing
// universes reproducible enough to exercise; until then a hard crash is attributed by the
// surviving on-disk markers alone.
template <typename BuildFn> CampaignReport run(CampaignConfig cfg, BuildFn build_fn) {
    CampaignReport report;
    if (cfg.parallel == 0) cfg.parallel = 1;

    std::atomic<uint64_t> next_seed_index{0};
    std::mutex report_mutex;
    std::vector<detail::WorkerShard> shards(cfg.parallel);
    std::vector<WorkerSeedMarker> markers(cfg.parallel);

    if (!cfg.marker_dir.empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(cfg.marker_dir, ignored);
    }

    const auto merge_and_maybe_flush = [&](unsigned worker_id, bool run_failed) {
        detail::WorkerShard& shard = shards[worker_id];
        std::lock_guard<std::mutex> lock(report_mutex);
        for (Failure& failure : shard.findings) {
            report.findings.push_back(std::move(failure));
        }
        if (shard.failed_run) ++report.failed_runs;
        const bool merged_run_failed = shard.failed_run;
        shard.findings.clear();
        shard.failed_run = false;
        ++report.runs;
        // Failure-triggered flush: a finding reaches disk the moment its universe merges, so a
        // later crash cannot lose it — not just on the periodic cadence.
        if (!cfg.report_path.empty() &&
            (run_failed || merged_run_failed ||
             (cfg.flush_every != 0 && report.runs % cfg.flush_every == 0))) {
            detail::flush_report_to_disk(cfg.report_path, report, cfg.max_seeds_per_finding);
        }
    };

    const auto worker_loop = [&](unsigned worker_id) {
        WorkerSeedMarker& marker = markers[worker_id];
        for (;;) {
            const uint64_t index = next_seed_index.fetch_add(1);
            if (index >= cfg.trials) return;
            const uint64_t seed = cfg.base_seed + index;
            marker.publish(seed);
            detail::write_seed_marker(cfg.marker_dir, worker_id, seed);

            {
                Simulator sim(seed);
                Simulator::Scope scope(sim);
                build_fn(sim, seed);
                sim.run_until_quiescence();

                for (const Failure& failure : sim.findings()) {
                    shards[worker_id].findings.push_back(failure);
                }
                if (!sim.findings().empty()) shards[worker_id].failed_run = true;

                if (cfg.verify) {
                    Simulator sim2(seed);
                    Simulator::Scope scope2(sim2);
                    build_fn(sim2, seed);
                    sim2.run_until_quiescence();
                    if (sim.trace_hash() != sim2.trace_hash()) {
                        // A determinism-contract violation is itself a finding (§8). The
                        // diverging hash pair goes in the detail so the report names it.
                        shards[worker_id].findings.push_back(
                            Failure{.seed = seed,
                                    .assertion_id = "determinism.violation",
                                    .detail = "trace hash diverged on re-run: " +
                                              std::to_string(sim.trace_hash()) +
                                              " != " + std::to_string(sim2.trace_hash())});
                        shards[worker_id].failed_run = true;
                    }
                }
            } // Scopes restored, sims destroyed: the markers still name the seed until cleared.

            marker.clear();
            detail::remove_seed_marker(cfg.marker_dir, worker_id);
            const bool run_failed = !shards[worker_id].findings.empty();
            merge_and_maybe_flush(worker_id, run_failed);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(cfg.parallel);
    for (unsigned worker_id = 0; worker_id < cfg.parallel; ++worker_id) {
        workers.emplace_back(worker_loop, worker_id);
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    report.compute_never_hit();
    report.sort_for_stable_output();
    // Final flush: the periodic cadence may not have landed since the last merge.
    detail::flush_report_to_disk(cfg.report_path, report, cfg.max_seeds_per_finding);
    return report;
}

// Run exactly one universe with the given seed, using the same build_fn the campaign used
// (harness contract, §4: factor build_fn into a named function, not an inline lambda, or
// repro runs silently diverge from campaign runs). Prints any findings to stderr. Returns 0
// on a clean run, 1 if the universe produced a finding — so CI can pin a regression to a
// single seed. The repro execution must be bit-identical to the failing campaign execution
// (same binary, same seed — §8); any difference is itself a determinism bug.
template <typename BuildFn> int run_single(uint64_t seed, BuildFn build_fn) {
    Simulator sim(seed);
    Simulator::Scope scope(sim);
    build_fn(sim, seed);
    sim.run_until_quiescence();

    for (const Failure& failure : sim.findings()) {
        std::fprintf(stderr, "FINDING: %s (seed %llu): %s\n", failure.assertion_id.c_str(),
                     static_cast<unsigned long long>(failure.seed), failure.detail.c_str());
    }
    return sim.findings().empty() ? 0 : 1;
}

} // namespace cosmos
