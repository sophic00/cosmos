#pragma once

// The campaign runner (docs/design.md §13): many universes from one base seed, each with its own
// fault configuration, aggregated into a single findings report.
//
// A universe runs in a worker process, not in the caller's. A crashing universe must become a
// finding rather than take the campaign with it, and only a process boundary can contain a signal.
// The consequence is deliberate: writes to captured variables inside the body are not visible here,
// and its returned ScenarioReport is the only channel back.

#include "cosmos/assert.hpp"
#include "cosmos/ledger_print.hpp"
#include "cosmos/scenario.hpp"
#include "cosmos/simulator.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cosmos {

// Reserved ids: the harness's own findings, never an application's (scenario.hpp's convention).
inline constexpr const char* kCrashedUniverseId = "cosmos.campaign.crash";
inline constexpr const char* kThrewUniverseId = "cosmos.campaign.threw";
inline constexpr const char* kVerifyRefusedId = "cosmos.campaign.verify";
inline constexpr const char* kForkFailedId = "cosmos.campaign.fork";

// True for the harness's own findings, which carry no universe and so no repro line.
inline bool is_harness_id(const std::string& id) { return id.rfind("cosmos.", 0) == 0; }

// Runs one universe. The campaign owns the Simulator; the callee drives a Scenario over it, so its
// report comes back as the universe's result.
using UniverseFn = std::function<ScenarioReport(Simulator&, uint64_t universe_index)>;

struct CampaignConfig {
    uint64_t trials = 1000;
    uint64_t base_seed = 0;
    unsigned parallel = std::thread::hardware_concurrency();
    uint32_t node_count = 1;
    // design.md §15's double-run trace-hash check, which needs the P5 decision trace. Refused, not
    // ignored: a caller must not read a green campaign as a determinism guarantee.
    bool verify = false;
    // Named in the repro line so a finding can be re-run verbatim, e.g. "myapp_test".
    std::string program;
};

// One violated assertion, deduplicated by id -- never by fault set, which is not globally minimal
// (plan §P4, "do not key findings dedup on the minimal fault set").
struct Finding {
    std::string id;
    uint64_t index = 0; // universe index, half of the (base_seed, index) repro identity
    uint64_t seed = 0;  // universe_seed(base_seed, index)
    std::string detail;
    Time at{};
    uint64_t universes_failed = 0;
    bool rare = false;  // reproduces in under a third of the runs
    std::string ledger; // rendered ledger of the first universe that failed this id
};

struct CampaignReport {
    uint64_t runs = 0;
    uint64_t failed_runs = 0;
    // Universes that produced no report at all: killed by a signal, or exited without one.
    uint64_t crashed_runs = 0;
    bool refused = false; // the harness declined to run; findings hold the reason
    std::vector<Finding> findings;
    std::vector<std::string> never_hit; // ids no universe reported as hit, kept apart from findings
    SiteCounterMap eligible_calls{};
    SiteCounterMap injections{};
    uint64_t out_of_scope_assertions = 0;

    // Echoed from the config so a report prints its own repro line without the caller's help.
    uint64_t base_seed = 0;
    std::string program;
};

namespace detail {

// One universe's result, in the form a parent process can fold after the universe is gone.
struct UniverseOutcome {
    uint64_t index = 0;
    uint64_t seed = 0;
    bool died = false; // produced no report: a signal, or an exit without one
    int signal = 0;    // WTERMSIG when it died on a signal
    std::string threw; // nonempty when the universe's body threw instead of reporting
    uint64_t out_of_scope = 0;
    ScenarioReport report{};
};

// Runs one universe in this process: the Simulator is per universe, so no state crosses between
// them.
inline UniverseOutcome run_one_universe(uint64_t index, uint64_t seed, const UniverseFn& build_fn) {
    UniverseOutcome outcome;
    outcome.index = index;
    outcome.seed = seed;
    const uint64_t out_of_scope_before = out_of_scope_assertions().count;
    {
        Simulator sim(seed);
        try {
            outcome.report = build_fn(sim, index);
        } catch (const std::exception& error) {
            outcome.threw = error.what();
        } catch (...) {
            outcome.threw = "unknown exception";
        }
        // Rendered here because the injector dies with the Simulator, and a finding needs its
        // ledger.
        if (!outcome.report.no_check_failed) {
            if (const auto* injector = sim.injector_or_null()) {
                std::ostringstream dump;
                print_ledger(dump, *injector);
                outcome.report.ledger_dump = dump.str();
            }
        }
    }
    outcome.out_of_scope = out_of_scope_assertions().count - out_of_scope_before;
    return outcome;
}

// The only place a finding is created or merged, so sequential and parallel runs cannot diverge.
class CampaignAccumulator {
  public:
    // Outcomes must arrive in ascending index so the "first universe" evidence is the lowest one.
    void add(const UniverseOutcome& outcome) {
        ++report_.runs;
        if (outcome.died) {
            ++report_.crashed_runs;
            ++report_.failed_runs;
            Finding& finding = find_or_add(kCrashedUniverseId, outcome);
            if (finding.universes_failed == 0) {
                finding.detail = outcome.signal != 0
                                     ? "killed by signal " + std::to_string(outcome.signal)
                                     : "exited without reporting";
            }
            ++finding.universes_failed;
            return;
        }
        if (!outcome.threw.empty()) {
            ++report_.failed_runs;
            Finding& finding = find_or_add(kThrewUniverseId, outcome);
            if (finding.universes_failed == 0) finding.detail = outcome.threw;
            ++finding.universes_failed;
            return;
        }

        const ScenarioReport& universe = outcome.report;
        if (!universe.no_check_failed) ++report_.failed_runs;
        for (const CheckResult& check : universe.checks) {
            if (check.passed) continue;
            Finding& finding = find_or_add(check.id, outcome);
            if (finding.universes_failed == 0) {
                finding.detail = check.detail;
                finding.at = check.at;
                finding.ledger = universe.ledger_dump;
            }
            ++finding.universes_failed;
        }
        // Coalesced by id, because a hit anywhere outweighs a miss everywhere.
        for (const CoverageNote& note : universe.coverage) {
            covered_[note.id] = covered_[note.id] || note.hit;
        }
        for (size_t slot = 0; slot < kSiteCount; ++slot) {
            report_.eligible_calls[slot] += universe.eligible_calls[slot];
            report_.injections[slot] += universe.injections[slot];
        }
        report_.out_of_scope_assertions += outcome.out_of_scope;
    }

    // A complaint about the campaign itself, belonging to no single universe.
    void add_harness_finding(const char* id, std::string detail) {
        Finding& finding = find_or_add(id, UniverseOutcome{});
        finding.detail = std::move(detail);
        ++finding.universes_failed;
    }

    // std::map's ordering makes the findings and never_hit sequences a function of the universe set
    // alone, which is what lets a parallel run match a sequential one exactly.
    [[nodiscard]] CampaignReport finish() const {
        CampaignReport report = report_;
        report.findings.reserve(findings_.size());
        for (const auto& [id, finding] : findings_) {
            Finding copy = finding;
            copy.rare = copy.universes_failed * 3 < report.runs;
            report.findings.push_back(std::move(copy));
        }
        for (const auto& [id, hit] : covered_) {
            if (!hit) report.never_hit.push_back(id);
        }
        return report;
    }

  private:
    Finding& find_or_add(const std::string& id, const UniverseOutcome& outcome) {
        auto [position, inserted] = findings_.try_emplace(id);
        if (inserted) {
            position->second.id = id;
            position->second.index = outcome.index;
            position->second.seed = outcome.seed;
        }
        return position->second;
    }

    std::map<std::string, Finding> findings_;
    std::map<std::string, bool> covered_;
    CampaignReport report_{};
};

// ---- the worker channel ----------------------------------------------------
//
// Both ends are the same binary, so fields are written as raw host-order bytes and the payload is
// length-prefixed. An anonymous file rather than a pipe: a regular file cannot fill up, so a worker
// never blocks on a reader that is still waiting for it, and unlinking up front means nothing to
// clean up even when a worker dies mid-write.

template <typename T> void put_field(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

inline void put_text(std::string& bytes, const std::string& text) {
    put_field(bytes, static_cast<uint64_t>(text.size()));
    bytes.append(text);
}

inline std::string serialize_outcome(const UniverseOutcome& outcome) {
    std::string bytes;
    put_field(bytes, outcome.index);
    put_field(bytes, outcome.seed);
    put_field(bytes, static_cast<uint8_t>(outcome.died ? 1 : 0));
    put_field(bytes, static_cast<int32_t>(outcome.signal));
    put_text(bytes, outcome.threw);
    put_field(bytes, outcome.out_of_scope);

    const ScenarioReport& report = outcome.report;
    put_field(bytes, report.seed);
    put_field(bytes, static_cast<uint8_t>(report.no_check_failed ? 1 : 0));
    put_field(bytes, static_cast<uint64_t>(report.checks.size()));
    for (const CheckResult& check : report.checks) {
        put_text(bytes, check.id);
        put_text(bytes, check.detail);
        put_field(bytes, static_cast<uint8_t>(check.passed ? 1 : 0));
        put_field(bytes, check.at.ns);
    }
    put_field(bytes, static_cast<uint64_t>(report.coverage.size()));
    for (const CoverageNote& note : report.coverage) {
        put_text(bytes, note.id);
        put_field(bytes, static_cast<uint8_t>(note.hit ? 1 : 0));
    }
    for (uint64_t count : report.eligible_calls)
        put_field(bytes, count);
    for (uint64_t count : report.injections)
        put_field(bytes, count);
    put_field(bytes, static_cast<uint64_t>(report.active_allocations));
    put_text(bytes, report.ledger_dump);
    return bytes;
}

// Walks a worker's bytes, stopping at a clean end or a half-written record.
class OutcomeReader {
  public:
    explicit OutcomeReader(std::string bytes) : bytes_(std::move(bytes)) {}

    bool next(UniverseOutcome& outcome) {
        if (offset_ >= bytes_.size()) return false;
        UniverseOutcome parsed;
        uint8_t flag = 0;
        int32_t signal = 0;
        uint64_t count = 0;

        if (!take(parsed.index) || !take(parsed.seed) || !take(flag) || !take(signal)) return false;
        parsed.died = flag != 0;
        parsed.signal = signal;
        if (!take_text(parsed.threw) || !take(parsed.out_of_scope)) return false;
        if (!take(parsed.report.seed) || !take(flag)) return false;
        parsed.report.no_check_failed = flag != 0;
        if (!take(count)) return false;
        for (uint64_t i = 0; i < count; ++i) {
            CheckResult check;
            if (!take_text(check.id) || !take_text(check.detail) || !take(flag)) return false;
            check.passed = flag != 0;
            if (!take(check.at.ns)) return false;
            parsed.report.checks.push_back(std::move(check));
        }
        if (!take(count)) return false;
        for (uint64_t i = 0; i < count; ++i) {
            CoverageNote note;
            if (!take_text(note.id) || !take(flag)) return false;
            note.hit = flag != 0;
            parsed.report.coverage.push_back(std::move(note));
        }
        for (uint64_t& value : parsed.report.eligible_calls) {
            if (!take(value)) return false;
        }
        for (uint64_t& value : parsed.report.injections) {
            if (!take(value)) return false;
        }
        if (!take(parsed.report.active_allocations)) return false;
        if (!take_text(parsed.report.ledger_dump)) return false;

        outcome = std::move(parsed);
        return true;
    }

  private:
    template <typename T> bool take(T& value) {
        if (offset_ + sizeof(T) > bytes_.size()) return false;
        std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    bool take_text(std::string& text) {
        uint64_t length = 0;
        if (!take(length)) return false;
        if (offset_ + length > bytes_.size()) return false;
        text.assign(bytes_, offset_, static_cast<size_t>(length));
        offset_ += static_cast<size_t>(length);
        return true;
    }

    std::string bytes_;
    size_t offset_ = 0;
};

// Unlinked on creation: the worker's death leaves nothing behind, and no reader can be blocked.
inline int open_scratch_file() {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string path = std::string(tmpdir != nullptr ? tmpdir : "/tmp") + "/cosmos-campaign-XXXXXX";
    const int fd = ::mkstemp(path.data());
    if (fd >= 0) ::unlink(path.c_str());
    return fd;
}

// Read after the worker is reaped; the child shares the file offset, so seeking first is required.
inline std::string read_scratch_file(int fd) {
    std::string bytes;
    if (::lseek(fd, 0, SEEK_SET) < 0) return bytes;
    char chunk[4096];
    for (;;) {
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got <= 0) break;
        bytes.append(chunk, static_cast<size_t>(got));
    }
    return bytes;
}

// Contiguous, so folding workers in order folds universes in order.
struct Slice {
    uint64_t begin = 0;
    uint64_t end = 0;
};

inline Slice slice_for(uint64_t trials, unsigned workers, unsigned worker) {
    return Slice{trials * worker / workers, trials * (worker + 1) / workers};
}

inline void run_slice_into(const Slice& slice, uint64_t base_seed, const UniverseFn& build_fn,
                           CampaignAccumulator& accumulator) {
    for (uint64_t index = slice.begin; index < slice.end; ++index) {
        accumulator.add(run_one_universe(index, universe_seed(base_seed, index), build_fn));
    }
}

// The child's whole job. Nothing here may touch the parent's memory and expect it to survive.
inline void run_slice_to_file(const Slice& slice, uint64_t base_seed, const UniverseFn& build_fn,
                              int fd) {
    std::string bytes;
    for (uint64_t index = slice.begin; index < slice.end; ++index) {
        bytes +=
            serialize_outcome(run_one_universe(index, universe_seed(base_seed, index), build_fn));
        if (bytes.size() >= 1u << 20) {
            const ssize_t written = ::write(fd, bytes.data(), bytes.size());
            if (written != static_cast<ssize_t>(bytes.size())) return;
            bytes.clear();
        }
    }
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (written <= 0) return;
        done += static_cast<size_t>(written);
    }
}

// One universe per worker for the tail of a slice whose worker died. A universe that dies again is
// attributed to its own seed, which is the whole point of paying for a second fork.
inline void recover_slice(uint64_t from, uint64_t to, uint64_t base_seed,
                          const UniverseFn& build_fn, CampaignAccumulator& accumulator) {
    for (uint64_t index = from; index < to; ++index) {
        UniverseOutcome outcome;
        const int fd = open_scratch_file();
        const pid_t pid = fd < 0 ? -1 : ::fork();
        if (pid == 0) {
            run_slice_to_file(Slice{index, index + 1}, base_seed, build_fn, fd);
            ::_exit(0);
        }
        if (pid < 0) {
            if (fd >= 0) ::close(fd);
            accumulator.add(run_one_universe(index, universe_seed(base_seed, index), build_fn));
            continue;
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        OutcomeReader reader(read_scratch_file(fd));
        if (!reader.next(outcome)) {
            outcome.index = index;
            outcome.seed = universe_seed(base_seed, index);
            outcome.died = true;
            outcome.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        }
        ::close(fd);
        accumulator.add(outcome);
    }
}

} // namespace detail

// Refusing costs nothing and says so; running anyway would hand back a green campaign that never
// checked what the flag names.
inline CampaignReport refused_report(const CampaignConfig& config, const char* detail) {
    CampaignReport report;
    report.refused = true;
    report.base_seed = config.base_seed;
    report.program = config.program;
    Finding finding;
    finding.id = kVerifyRefusedId;
    finding.detail = detail;
    report.findings.push_back(std::move(finding));
    return report;
}

// §17.4's shape, plus the repro line §15 promises. Findings and never_hit print in id order.
inline void print_campaign_report(std::ostream& out, const CampaignReport& report) {
    if (report.refused) {
        out << "REFUSED (" << report.runs << " universes run)\n";
    } else {
        out << "Campaign: " << report.runs << " universes, " << report.failed_runs << " failed, "
            << report.crashed_runs << " crashed\n";
    }
    for (const Finding& finding : report.findings) {
        out << "FINDING \"" << finding.id << "\"";
        if (!is_harness_id(finding.id)) {
            out << (finding.rare ? "  rare" : "") << "  universes = " << finding.universes_failed;
        }
        out << "\n";
        if (!finding.detail.empty()) out << "  detail  = \"" << finding.detail << "\"\n";
        if (finding.seed != 0) {
            out << "  t       = " << detail::format_ms(finding.at) << "\n";
            out << "  repro   = ";
            if (!report.program.empty()) out << report.program << " ";
            out << "--seed " << finding.seed << "   (base_seed " << report.base_seed << ", index "
                << finding.index << ")\n";
        }
        if (!finding.ledger.empty()) out << finding.ledger;
    }
    for (const std::string& id : report.never_hit) {
        out << "NEVER HIT: \"" << id << "\"\n";
    }
    if (report.out_of_scope_assertions > 0) {
        out << "OUT OF SCOPE: " << report.out_of_scope_assertions
            << " assertion(s) with no universe bound\n";
    }
}

namespace detail {

inline void json_escape(std::ostream& out, const std::string& text) {
    out << '"';
    for (const char raw : text) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (byte < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
                out << escaped;
            } else {
                out << raw;
            }
        }
    }
    out << '"';
}

// Paired rather than two flat maps: a site's eligible count means little without its fire count.
inline void json_sites(std::ostream& out, const CampaignReport& report) {
    out << "  \"sites\": {";
    for (size_t slot = 0; slot < kSiteCount; ++slot) {
        if (slot != 0) out << ", ";
        out << '"' << name_of(site_at_slot(slot))
            << "\": {\"eligible\": " << report.eligible_calls[slot]
            << ", \"fired\": " << report.injections[slot] << '}';
    }
    out << "},\n";
}

} // namespace detail

// The dashboard's input (plan §P4): findings, never_hit kept in its own section, per-site counters
// and the repro seeds. Written to a stream, so the caller decides what to do with it.
inline void print_campaign_json(std::ostream& out, const CampaignReport& report) {
    out << "{\n";
    out << "  \"runs\": " << report.runs << ",\n";
    out << "  \"failed_runs\": " << report.failed_runs << ",\n";
    out << "  \"crashed_runs\": " << report.crashed_runs << ",\n";
    out << "  \"refused\": " << (report.refused ? "true" : "false") << ",\n";
    out << "  \"base_seed\": " << report.base_seed << ",\n";
    out << "  \"findings\": [";
    for (size_t i = 0; i < report.findings.size(); ++i) {
        const Finding& finding = report.findings[i];
        out << (i == 0 ? "\n    " : ",\n    ");
        out << "{\"id\": ";
        detail::json_escape(out, finding.id);
        out << ", \"index\": " << finding.index << ", \"seed\": " << finding.seed
            << ", \"universes_failed\": " << finding.universes_failed
            << ", \"rare\": " << (finding.rare ? "true" : "false") << ", \"detail\": ";
        detail::json_escape(out, finding.detail);
        out << ", \"ledger\": ";
        detail::json_escape(out, finding.ledger);
        out << '}';
    }
    out << (report.findings.empty() ? "],\n" : "\n  ],\n");

    out << "  \"never_hit\": [";
    for (size_t i = 0; i < report.never_hit.size(); ++i) {
        out << (i == 0 ? "\n    " : ",\n    ");
        detail::json_escape(out, report.never_hit[i]);
    }
    out << (report.never_hit.empty() ? "],\n" : "\n  ],\n");

    detail::json_sites(out, report);
    out << "  \"out_of_scope_assertions\": " << report.out_of_scope_assertions << "\n";
    out << "}\n";
}

class Campaign {
  public:
    static CampaignReport run(const CampaignConfig& config, const UniverseFn& build_fn) {
        if (config.verify) {
            return refused_report(config, "CampaignConfig::verify needs the P5 decision trace");
        }

        const unsigned workers = std::max(1u, config.parallel);
        struct LaunchedWorker {
            detail::Slice slice;
            int fd = -1;
            pid_t pid = -1;
        };
        std::vector<LaunchedWorker> launched;
        for (unsigned worker = 0; worker < workers; ++worker) {
            const detail::Slice slice = detail::slice_for(config.trials, workers, worker);
            if (slice.begin == slice.end) continue;
            const int fd = detail::open_scratch_file();
            const pid_t pid = fd < 0 ? -1 : ::fork();
            // The child runs its slice and leaves; _exit skips inherited stdio and atexit handlers.
            if (pid == 0) {
                detail::run_slice_to_file(slice, config.base_seed, build_fn, fd);
                ::_exit(0);
            }
            launched.push_back(LaunchedWorker{slice, fd, pid});
        }

        detail::CampaignAccumulator accumulator;
        for (const LaunchedWorker& worker : launched) {
            if (worker.pid > 0) {
                int status = 0;
                ::waitpid(worker.pid, &status, 0);
                detail::OutcomeReader reader(detail::read_scratch_file(worker.fd));
                uint64_t next = worker.slice.begin;
                detail::UniverseOutcome outcome;
                while (reader.next(outcome)) {
                    accumulator.add(outcome);
                    ++next;
                }
                // Shorter than the slice means the worker died or wrote short: recover the rest.
                if (next < worker.slice.end) {
                    detail::recover_slice(next, worker.slice.end, config.base_seed, build_fn,
                                          accumulator);
                }
            } else {
                accumulator.add_harness_finding(
                    kForkFailedId,
                    "fork() failed; that slice ran in-process and is not crash-contained");
                detail::run_slice_into(worker.slice, config.base_seed, build_fn, accumulator);
            }
            if (worker.fd >= 0) ::close(worker.fd);
        }

        CampaignReport report = accumulator.finish();
        report.base_seed = config.base_seed;
        report.program = config.program;
        return report;
    }
};

} // namespace cosmos
