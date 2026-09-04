#pragma once

#include "cosmos/random.hpp"
#include "cosmos/time.hpp"
#include <array>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <variant>
#include <vector>

namespace cosmos {

enum class FaultClass : uint8_t { Memory, Network, Storage, Clock, Process, Random, _Count };

constexpr size_t kFaultClassCount = static_cast<size_t>(FaultClass::_Count);

// Each class draws from its own sub-stream, so one class's config cannot shift another's (Rule 2).
inline uint64_t fault_class_seed(uint64_t fault_stream_seed, FaultClass fault_class) {
    return derive_seed(fault_stream_seed, static_cast<uint64_t>(fault_class));
}

enum class FaultMode : uint8_t { Safety, Liveness };

// Append-only: traces and repro commands key on these, so renumbering re-points history (Rule 13).
enum class SiteId : uint64_t {
    malloc = 1,
    calloc = 2,
    realloc = 3,
    open = 4,
    read = 5,
    write = 6,
    fsync = 7,
    connect = 8,
    accept = 9,
    send = 10,
    recv = 11,
    clock_gettime = 12,
    nanosleep = 13,
    getrandom = 14,
    crash_node = 1000,
    partition = 1001,
    pause_node = 1002,
};

constexpr size_t kWrapperSiteCount = 14;
constexpr size_t kEventSiteCount = 3;
constexpr size_t kSiteCount = kWrapperSiteCount + kEventSiteCount;
constexpr size_t kNoSite = static_cast<size_t>(-1);

using SiteCounterMap = std::array<uint64_t, kSiteCount>;

// Written out, not derived: the values are append-only but not required to stay contiguous.
constexpr size_t site_slot(SiteId site) {
    switch (site) {
    case SiteId::malloc:
        return 0;
    case SiteId::calloc:
        return 1;
    case SiteId::realloc:
        return 2;
    case SiteId::open:
        return 3;
    case SiteId::read:
        return 4;
    case SiteId::write:
        return 5;
    case SiteId::fsync:
        return 6;
    case SiteId::connect:
        return 7;
    case SiteId::accept:
        return 8;
    case SiteId::send:
        return 9;
    case SiteId::recv:
        return 10;
    case SiteId::clock_gettime:
        return 11;
    case SiteId::nanosleep:
        return 12;
    case SiteId::getrandom:
        return 13;
    case SiteId::crash_node:
        return 14;
    case SiteId::partition:
        return 15;
    case SiteId::pause_node:
        return 16;
    }
    return kNoSite;
}

constexpr std::array<SiteId, kSiteCount> kAllSites{
    SiteId::malloc,     SiteId::calloc,    SiteId::realloc,    SiteId::open,
    SiteId::read,       SiteId::write,     SiteId::fsync,      SiteId::connect,
    SiteId::accept,     SiteId::send,      SiteId::recv,       SiteId::clock_gettime,
    SiteId::nanosleep,  SiteId::getrandom, SiteId::crash_node, SiteId::partition,
    SiteId::pause_node,
};

constexpr SiteId site_at_slot(size_t slot) { return kAllSites[slot]; }

// -Werror=switch catches a site missing from site_slot(); a gap here would index past rules.
constexpr bool slots_are_consistent() {
    for (size_t slot = 0; slot < kSiteCount; ++slot) {
        if (site_slot(kAllSites[slot]) != slot) return false;
    }
    return true;
}
static_assert(slots_are_consistent(), "kAllSites and site_slot() disagree");

constexpr bool is_known_site(SiteId site) { return site_slot(site) != kNoSite; }

// Event sites have no call to wrap, so no rate and no trigger; they fire from episodes (§10.2).
constexpr bool is_event_site(SiteId site) {
    const size_t slot = site_slot(site);
    return slot != kNoSite && slot >= kWrapperSiteCount;
}

constexpr FaultClass class_of(SiteId site) {
    switch (site) {
    case SiteId::malloc:
    case SiteId::calloc:
    case SiteId::realloc:
        return FaultClass::Memory;
    case SiteId::open:
    case SiteId::read:
    case SiteId::write:
    case SiteId::fsync:
        return FaultClass::Storage;
    case SiteId::connect:
    case SiteId::accept:
    case SiteId::send:
    case SiteId::recv:
        return FaultClass::Network;
    case SiteId::clock_gettime:
    case SiteId::nanosleep:
        return FaultClass::Clock;
    case SiteId::getrandom:
        return FaultClass::Random;
    case SiteId::crash_node:
    case SiteId::partition:
    case SiteId::pause_node:
        return FaultClass::Process;
    }
    return FaultClass::_Count;
}

// The wrapper, never the injector, maps a kind to a legal result for its API (§8.2). Append-only.
enum class FaultKind : uint8_t {
    None = 0,
    OutOfMemory,
    OpenEio,
    ReadEio,
    WriteEio,
    ShortWrite,
    NoSpace,
    FsyncEio,
    ConnRefused,
    ConnReset,
    PeerClose,
    ShortSend,
    PacketDrop,
    PacketDelay,
    PacketReorder,
    PacketCorrupt,
    ClockStep,
    SleepInterrupted,
    RandomEagain,
    _Count,
};

// Rule 15: without this a rule can ask send() for ENOMEM, testing an impossible world.
constexpr bool is_legal_outcome(SiteId site, FaultKind kind) {
    switch (site) {
    case SiteId::malloc:
    case SiteId::calloc:
    case SiteId::realloc:
        return kind == FaultKind::OutOfMemory;
    case SiteId::open:
        return kind == FaultKind::OpenEio || kind == FaultKind::NoSpace;
    case SiteId::read:
        return kind == FaultKind::ReadEio;
    case SiteId::write:
        return kind == FaultKind::WriteEio || kind == FaultKind::ShortWrite ||
               kind == FaultKind::NoSpace;
    case SiteId::fsync:
        return kind == FaultKind::FsyncEio || kind == FaultKind::NoSpace;
    case SiteId::connect:
        return kind == FaultKind::ConnRefused || kind == FaultKind::ConnReset;
    case SiteId::accept:
        return kind == FaultKind::ConnReset;
    case SiteId::send:
        return kind == FaultKind::ConnReset || kind == FaultKind::ShortSend ||
               kind == FaultKind::PacketDrop || kind == FaultKind::PacketDelay ||
               kind == FaultKind::PacketReorder || kind == FaultKind::PacketCorrupt;
    case SiteId::recv:
        return kind == FaultKind::ConnReset || kind == FaultKind::PeerClose;
    case SiteId::clock_gettime:
        return kind == FaultKind::ClockStep;
    case SiteId::nanosleep:
        return kind == FaultKind::SleepInterrupted;
    case SiteId::getrandom:
        return kind == FaultKind::RandomEagain;
    case SiteId::crash_node:
    case SiteId::partition:
    case SiteId::pause_node:
        return false;
    }
    return false;
}

struct SiteOutcome {
    FaultKind kind = FaultKind::None;
    double weight = 0.0;

    constexpr bool operator==(const SiteOutcome&) const = default;
};

// Inline storage keeps a rule copy allocation-free; the engine runs inside __wrap_malloc.
constexpr size_t kMaxOutcomes = 6;

// A cap below what is_legal_outcome() permits would silently drop outcomes the user asked for.
constexpr size_t widest_legal_menu() {
    size_t worst = 0;
    for (SiteId site : kAllSites) {
        size_t legal = 0;
        for (uint8_t k = 1; k < static_cast<uint8_t>(FaultKind::_Count); ++k) {
            if (is_legal_outcome(site, static_cast<FaultKind>(k))) ++legal;
        }
        if (legal > worst) worst = legal;
    }
    return worst;
}
static_assert(kMaxOutcomes >= widest_legal_menu(),
              "kMaxOutcomes cannot hold every outcome is_legal_outcome() permits");

struct OutcomeTable {
    std::array<SiteOutcome, kMaxOutcomes> entries{};
    size_t count = 0;

    constexpr bool empty() const { return count == 0; }
    constexpr bool full() const { return count == kMaxOutcomes; }

    // False means the table was full: a silently shortened table is a different configuration.
    [[nodiscard]] constexpr bool add(FaultKind kind, double weight) {
        if (full()) return false;
        entries[count] = SiteOutcome{kind, weight};
        ++count;
        return true;
    }

    constexpr const SiteOutcome* begin() const { return entries.data(); }
    constexpr const SiteOutcome* end() const { return entries.data() + count; }

    constexpr bool operator==(const OutcomeTable& other) const {
        if (count != other.count) return false;
        for (size_t i = 0; i < count; ++i) {
            if (!(entries[i] == other.entries[i])) return false;
        }
        return true;
    }
};

struct FaultRule {
    double rate = 0.0;
    uint64_t skip_first = 0;
    uint64_t max_injections = UINT64_MAX;
    OutcomeTable outcomes{};
    // Wrapper sites only; 1-based count of eligible calls (§10.1).
    std::optional<uint64_t> fire_on_eligible_call{};
};

enum class ConfigError : uint8_t {
    BadRate,
    BadWeight,
    BadOutcomeKind,
    IllegalOutcome,
    EmptyOutcomes,
    TriggerLeSkipFirst,
    TriggerOnEventSite,
    RuleOnEventSite,
    RuleOnDisabledClass,
    EpisodeOutsideWindows,
    BadEpisodeDuration,
    UnknownNode,
    BadKnobOrder,
    QuorumExceedsNodes,
    LimitsExceedNodes,
    BadWindowOrder,
    // Not a config defect: this build fires no triggers yet, so the promise would go unmet.
    TriggerNotImplemented,
};

// Carries the offending site so a rejected config says where, not just what.
struct ConfigProblem {
    ConfigError error;
    std::optional<SiteId> site{};

    constexpr bool operator==(const ConfigProblem&) const = default;
};

using NodeId = uint32_t;
using NodeSet = std::vector<NodeId>;
using KnobId = uint64_t;

struct CrashNode {
    NodeId id;
};
struct Partition {
    NodeSet a, b;
};
struct PauseNode {
    NodeId id;
};
using EpisodeSpec = std::variant<CrashNode, Partition, PauseNode>;

struct ScheduledEpisode {
    Time at{};
    EpisodeSpec spec{CrashNode{0}};
    Duration until_heal{};
};

struct Knob {
    KnobId id = 0;
    int64_t value = 0;
};

// Pure data, sampled once per universe; copyable so a failing run can report it.
struct FaultConfig {
    FaultMode mode = FaultMode::Safety;

    std::bitset<kFaultClassCount> enabled{};
    std::bitset<kSiteCount> activated_sites{};

    // Fixed storage, not a hash map: iteration order is what the swarm sampler draws against
    // (Rules 4 and 8).
    std::array<std::optional<FaultRule>, kSiteCount> rules{};

    uint32_t max_crashed_nodes = 0;
    uint32_t min_healthy_quorum = 0;

    std::vector<ScheduledEpisode> scheduled_episodes{};
    std::vector<Knob> knobs{};

    Time warmup_until = Time::zero();
    Time quiesce_after = Time::max();

    bool is_class_enabled(FaultClass c) const { return enabled.test(static_cast<size_t>(c)); }

    void enable_class(FaultClass c) { enabled.set(static_cast<size_t>(c)); }

    bool is_site_activated(SiteId site) const {
        const size_t slot = site_slot(site);
        return slot != kNoSite && activated_sites.test(slot);
    }

    // False means the build does not know the site; P5 replay parses ids from a trace.
    [[nodiscard]] bool activate_site(SiteId site) {
        const size_t slot = site_slot(site);
        if (slot == kNoSite) return false;
        activated_sites.set(slot);
        return true;
    }

    const FaultRule* rule_for(SiteId site) const {
        const size_t slot = site_slot(site);
        if (slot == kNoSite || !rules[slot].has_value()) return nullptr;
        return &*rules[slot];
    }

    [[nodiscard]] bool set_rule(SiteId site, FaultRule rule) {
        const size_t slot = site_slot(site);
        if (slot == kNoSite) return false;
        rules[slot] = std::move(rule);
        return true;
    }

    // Checks only; normalization is separate so a checker cannot rewrite the config it inspects.
    // Reports the first problem in a fixed order, so a NaN rate on a disabled class reports
    // RuleOnDisabledClass rather than BadRate.
    [[nodiscard]] std::expected<void, ConfigProblem> validate(uint32_t node_count) const;

    // Scales tables to sum to 1, like std::discrete_distribution; called on the injector's copy.
    void normalize();
};

inline bool is_finite_rate(double v) { return std::isfinite(v) && v >= 0.0 && v <= 1.0; }

inline std::expected<void, ConfigProblem> FaultConfig::validate(uint32_t node_count) const {
    if (warmup_until > quiesce_after) {
        return std::unexpected(ConfigProblem{ConfigError::BadWindowOrder, std::nullopt});
    }
    if (min_healthy_quorum > node_count || max_crashed_nodes > node_count) {
        return std::unexpected(ConfigProblem{ConfigError::QuorumExceedsNodes, std::nullopt});
    }
    // Crashing N while demanding M healthy with N + M > node_count contradicts itself (§2). Summed
    // as 64-bit so two large uint32 limits cannot wrap into a value that passes.
    if (static_cast<uint64_t>(max_crashed_nodes) + min_healthy_quorum > node_count) {
        return std::unexpected(ConfigProblem{ConfigError::LimitsExceedNodes, std::nullopt});
    }

    for (size_t slot = 0; slot < kSiteCount; ++slot) {
        if (!rules[slot].has_value()) continue;

        const FaultRule& rule = *rules[slot];
        const SiteId site = site_at_slot(slot);

        if (slot >= kWrapperSiteCount) {
            const ConfigError error = rule.fire_on_eligible_call.has_value()
                                          ? ConfigError::TriggerOnEventSite
                                          : ConfigError::RuleOnEventSite;
            return std::unexpected(ConfigProblem{error, site});
        }

        if (!is_class_enabled(class_of(site))) {
            return std::unexpected(ConfigProblem{ConfigError::RuleOnDisabledClass, site});
        }
        if (!is_finite_rate(rule.rate)) {
            return std::unexpected(ConfigProblem{ConfigError::BadRate, site});
        }

        const bool can_fire = rule.rate > 0.0 || rule.fire_on_eligible_call.has_value();
        if (can_fire && rule.outcomes.empty()) {
            return std::unexpected(ConfigProblem{ConfigError::EmptyOutcomes, site});
        }
        for (const SiteOutcome& outcome : rule.outcomes) {
            if (outcome.kind == FaultKind::None) {
                return std::unexpected(ConfigProblem{ConfigError::BadOutcomeKind, site});
            }
            if (!std::isfinite(outcome.weight) || outcome.weight <= 0.0) {
                return std::unexpected(ConfigProblem{ConfigError::BadWeight, site});
            }
            if (!is_legal_outcome(site, outcome.kind)) {
                return std::unexpected(ConfigProblem{ConfigError::IllegalOutcome, site});
            }
        }
        if (rule.fire_on_eligible_call.has_value() &&
            *rule.fire_on_eligible_call <= rule.skip_first) {
            return std::unexpected(ConfigProblem{ConfigError::TriggerLeSkipFirst, site});
        }
    }

    for (const ScheduledEpisode& episode : scheduled_episodes) {
        if (episode.at < warmup_until || episode.at >= quiesce_after) {
            return std::unexpected(ConfigProblem{ConfigError::EpisodeOutsideWindows, std::nullopt});
        }
        // Rule 5: a zero-length episode heals as it starts, so the application never observes it.
        if (episode.until_heal <= Duration::zero()) {
            return std::unexpected(ConfigProblem{ConfigError::BadEpisodeDuration, std::nullopt});
        }

        const ConfigProblem unknown_node{ConfigError::UnknownNode, std::nullopt};
        if (const auto* crash = std::get_if<CrashNode>(&episode.spec)) {
            if (crash->id >= node_count) return std::unexpected(unknown_node);
        } else if (const auto* pause = std::get_if<PauseNode>(&episode.spec)) {
            if (pause->id >= node_count) return std::unexpected(unknown_node);
        } else if (const auto* split = std::get_if<Partition>(&episode.spec)) {
            for (NodeId id : split->a) {
                if (id >= node_count) return std::unexpected(unknown_node);
            }
            for (NodeId id : split->b) {
                if (id >= node_count) return std::unexpected(unknown_node);
            }
        }
    }

    // Knobs are drawn per id, so order is part of the reproduction contract; no duplicates.
    for (size_t i = 1; i < knobs.size(); ++i) {
        if (knobs[i - 1].id >= knobs[i].id) {
            return std::unexpected(ConfigProblem{ConfigError::BadKnobOrder, std::nullopt});
        }
    }

    return {};
}

inline void FaultConfig::normalize() {
    for (std::optional<FaultRule>& slot : rules) {
        if (!slot.has_value()) continue;

        OutcomeTable& table = slot->outcomes;
        double total = 0.0;
        for (size_t i = 0; i < table.count; ++i) {
            total += table.entries[i].weight;
        }
        // validate() rejects non-finite totals, so leave them rather than zeroing every weight.
        if (!std::isfinite(total) || total <= 0.0) continue;
        for (size_t i = 0; i < table.count; ++i) {
            table.entries[i].weight /= total;
        }
    }
}

// Superseded by FaultConfig/FaultRule; deleted in P2-S1 once wrap_memory.cpp is rewired.
struct FaultProfile {
    double oom_rate = 0.0; // Heap allocation failure probability [0.0, 1.0]

    // Endpoint rates never draw (Rule 3): only intermediate rates consume a decision.
    bool should_inject_oom(Rng& rng) const {
        if (oom_rate <= 0.0) return false;
        if (oom_rate >= 1.0) return true;
        return rng.uniform() < oom_rate;
    }
};

} // namespace cosmos
