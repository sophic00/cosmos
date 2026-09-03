#include "cosmos/faults.hpp"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

using namespace cosmos::literals;
using cosmos::ConfigError;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultKind;
using cosmos::FaultRule;
using cosmos::SiteId;

namespace {

constexpr uint32_t kNodes = 3;

// The setters and OutcomeTable::add report failure rather than silently doing nothing.
void must(bool ok) { assert(ok); }

// Rule 13: these values are burned into every ledger, decision trace and repro command ever
// emitted. Renumbering one silently re-points historical repros at a different site.
static_assert(static_cast<uint64_t>(SiteId::malloc) == 1);
static_assert(static_cast<uint64_t>(SiteId::calloc) == 2);
static_assert(static_cast<uint64_t>(SiteId::realloc) == 3);
static_assert(static_cast<uint64_t>(SiteId::open) == 4);
static_assert(static_cast<uint64_t>(SiteId::read) == 5);
static_assert(static_cast<uint64_t>(SiteId::write) == 6);
static_assert(static_cast<uint64_t>(SiteId::fsync) == 7);
static_assert(static_cast<uint64_t>(SiteId::connect) == 8);
static_assert(static_cast<uint64_t>(SiteId::accept) == 9);
static_assert(static_cast<uint64_t>(SiteId::send) == 10);
static_assert(static_cast<uint64_t>(SiteId::recv) == 11);
static_assert(static_cast<uint64_t>(SiteId::clock_gettime) == 12);
static_assert(static_cast<uint64_t>(SiteId::nanosleep) == 13);
static_assert(static_cast<uint64_t>(SiteId::getrandom) == 14);
static_assert(static_cast<uint64_t>(SiteId::crash_node) == 1000);
static_assert(static_cast<uint64_t>(SiteId::partition) == 1001);
static_assert(static_cast<uint64_t>(SiteId::pause_node) == 1002);

static_assert(cosmos::kWrapperSiteCount == 14);
static_assert(cosmos::kEventSiteCount == 3);
static_assert(cosmos::site_slot(SiteId::malloc) == 0);
static_assert(cosmos::site_slot(SiteId::getrandom) == 13);
static_assert(cosmos::site_slot(SiteId::crash_node) == 14);
static_assert(cosmos::site_slot(SiteId::pause_node) == 16);
static_assert(cosmos::site_slot(static_cast<SiteId>(9999)) == cosmos::kNoSite);

static_assert(!cosmos::is_event_site(SiteId::malloc));
static_assert(cosmos::is_event_site(SiteId::crash_node));
static_assert(cosmos::class_of(SiteId::malloc) == FaultClass::Memory);
static_assert(cosmos::class_of(SiteId::write) == FaultClass::Storage);
static_assert(cosmos::class_of(SiteId::send) == FaultClass::Network);
static_assert(cosmos::class_of(SiteId::clock_gettime) == FaultClass::Clock);
static_assert(cosmos::class_of(SiteId::getrandom) == FaultClass::Random);
static_assert(cosmos::class_of(SiteId::crash_node) == FaultClass::Process);

FaultRule oom_rule(double rate) {
    FaultRule rule;
    rule.rate = rate;
    must(rule.outcomes.add(FaultKind::OutOfMemory, 1.0));
    return rule;
}

// Two classes, several sites, a multi-outcome table, windows, limits and an episode in range.
FaultConfig valid_config() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Memory);
    cfg.enable_class(FaultClass::Network);
    cfg.enable_class(FaultClass::Process);

    must(cfg.activate_site(SiteId::malloc));
    must(cfg.activate_site(SiteId::send));
    must(cfg.activate_site(SiteId::crash_node));

    FaultRule malloc_rule = oom_rule(0.01);
    malloc_rule.skip_first = 100;
    malloc_rule.max_injections = 5;
    must(cfg.set_rule(SiteId::malloc, malloc_rule));

    FaultRule send_rule;
    send_rule.rate = 0.05;
    must(send_rule.outcomes.add(FaultKind::PacketDrop, 1.0));
    must(send_rule.outcomes.add(FaultKind::PacketDelay, 1.0));
    must(send_rule.outcomes.add(FaultKind::PacketCorrupt, 2.0));
    must(cfg.set_rule(SiteId::send, send_rule));

    cfg.max_crashed_nodes = 1;
    cfg.min_healthy_quorum = 2;
    cfg.warmup_until = cosmos::Time::zero() + 10_ms;
    cfg.quiesce_after = cosmos::Time::zero() + 900_ms;
    cfg.scheduled_episodes.push_back({cosmos::Time::zero() + 30_ms, cosmos::CrashNode{2}, 2_s});
    cfg.knobs.push_back({1, 100});
    return cfg;
}

void assert_rejects(const FaultConfig& cfg, ConfigError expected) {
    auto result = cfg.validate(kNodes);
    assert(!result.has_value());
    assert(result.error().error == expected);
}

} // namespace

void test_valid_config_passes() {
    assert(valid_config().validate(kNodes).has_value());

    FaultConfig empty;
    assert(empty.validate(kNodes).has_value());

    std::cout << "[PASS] test_valid_config_passes" << std::endl;
}

void test_bad_rates_are_rejected() {
    for (double rate : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::infinity()}) {
        FaultConfig cfg = valid_config();
        must(cfg.set_rule(SiteId::malloc, oom_rule(rate)));
        assert_rejects(cfg, ConfigError::BadRate);
    }

    // The boundaries themselves are legal.
    for (double rate : {0.0, 1.0}) {
        FaultConfig cfg = valid_config();
        must(cfg.set_rule(SiteId::malloc, oom_rule(rate)));
        assert(cfg.validate(kNodes).has_value());
    }

    std::cout << "[PASS] test_bad_rates_are_rejected" << std::endl;
}

void test_bad_outcome_tables_are_rejected() {
    FaultConfig cfg = valid_config();
    FaultRule no_outcomes;
    no_outcomes.rate = 0.5;
    must(cfg.set_rule(SiteId::malloc, no_outcomes));
    assert_rejects(cfg, ConfigError::EmptyOutcomes);

    // A trigger can fire without a rate, so it needs an outcome just the same.
    FaultConfig triggered = valid_config();
    FaultRule trigger_only;
    trigger_only.fire_on_eligible_call = 10;
    must(triggered.set_rule(SiteId::malloc, trigger_only));
    assert_rejects(triggered, ConfigError::EmptyOutcomes);

    for (double weight : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        FaultConfig bad = valid_config();
        FaultRule rule;
        rule.rate = 0.5;
        must(rule.outcomes.add(FaultKind::OutOfMemory, weight));
        must(bad.set_rule(SiteId::malloc, rule));
        assert_rejects(bad, ConfigError::BadWeight);
    }

    // A rate of zero with no trigger can never fire, so an empty table is fine.
    FaultConfig inert = valid_config();
    must(inert.set_rule(SiteId::malloc, FaultRule{}));
    assert(inert.validate(kNodes).has_value());

    std::cout << "[PASS] test_bad_outcome_tables_are_rejected" << std::endl;
}

void test_trigger_must_exceed_skip_first() {
    for (uint64_t trigger : {uint64_t(3), uint64_t(2)}) {
        FaultConfig cfg = valid_config();
        FaultRule rule = oom_rule(0.0);
        rule.skip_first = 3;
        rule.fire_on_eligible_call = trigger;
        must(cfg.set_rule(SiteId::malloc, rule));
        assert_rejects(cfg, ConfigError::TriggerLeSkipFirst);
    }

    FaultConfig ok = valid_config();
    FaultRule rule = oom_rule(0.0);
    rule.skip_first = 3;
    rule.fire_on_eligible_call = 4;
    must(ok.set_rule(SiteId::malloc, rule));
    assert(ok.validate(kNodes).has_value());

    std::cout << "[PASS] test_trigger_must_exceed_skip_first" << std::endl;
}

void test_event_sites_reject_rules_and_triggers() {
    FaultConfig with_trigger = valid_config();
    FaultRule triggered = oom_rule(0.0);
    triggered.fire_on_eligible_call = 443;
    must(with_trigger.set_rule(SiteId::crash_node, triggered));
    assert_rejects(with_trigger, ConfigError::TriggerOnEventSite);
    assert(with_trigger.validate(kNodes).error().site == SiteId::crash_node);

    // Event sites carry no rate either: they fire when scheduled or invoked (§10.2).
    FaultConfig with_rate = valid_config();
    must(with_rate.set_rule(SiteId::partition, oom_rule(0.5)));
    assert_rejects(with_rate, ConfigError::RuleOnEventSite);

    std::cout << "[PASS] test_event_sites_reject_rules_and_triggers" << std::endl;
}

void test_rule_on_disabled_class_is_rejected() {
    FaultConfig cfg = valid_config();
    must(cfg.activate_site(SiteId::write));
    FaultRule write_rule;
    write_rule.rate = 0.01;
    must(write_rule.outcomes.add(FaultKind::WriteEio, 1.0));
    must(cfg.set_rule(SiteId::write, write_rule));
    assert_rejects(cfg, ConfigError::RuleOnDisabledClass);
    assert(cfg.validate(kNodes).error().site == SiteId::write);

    cfg.enable_class(FaultClass::Storage);
    assert(cfg.validate(kNodes).has_value());

    std::cout << "[PASS] test_rule_on_disabled_class_is_rejected" << std::endl;
}

void test_windows_and_quorum_are_checked() {
    FaultConfig inverted = valid_config();
    inverted.warmup_until = cosmos::Time::zero() + 2_s;
    inverted.quiesce_after = cosmos::Time::zero() + 1_s;
    assert_rejects(inverted, ConfigError::BadWindowOrder);

    FaultConfig quorum = valid_config();
    quorum.min_healthy_quorum = kNodes + 1;
    assert_rejects(quorum, ConfigError::QuorumExceedsNodes);

    FaultConfig early = valid_config();
    early.scheduled_episodes.clear();
    early.scheduled_episodes.push_back({cosmos::Time::zero(), cosmos::CrashNode{1}, 1_s});
    assert_rejects(early, ConfigError::EpisodeOutsideWindows);

    FaultConfig late = valid_config();
    late.scheduled_episodes.clear();
    late.scheduled_episodes.push_back({cosmos::Time::zero() + 5_s, cosmos::CrashNode{1}, 1_s});
    assert_rejects(late, ConfigError::EpisodeOutsideWindows);

    std::cout << "[PASS] test_windows_and_quorum_are_checked" << std::endl;
}

void test_weight_normalization_is_scale_invariant() {
    FaultConfig fractional = valid_config();
    FaultRule a;
    a.rate = 0.5;
    must(a.outcomes.add(FaultKind::PacketDrop, 0.5));
    must(a.outcomes.add(FaultKind::PacketDelay, 0.3));
    must(a.outcomes.add(FaultKind::PacketCorrupt, 0.2));
    must(fractional.set_rule(SiteId::send, a));

    FaultConfig integral = valid_config();
    FaultRule b;
    b.rate = 0.5;
    must(b.outcomes.add(FaultKind::PacketDrop, 5.0));
    must(b.outcomes.add(FaultKind::PacketDelay, 3.0));
    must(b.outcomes.add(FaultKind::PacketCorrupt, 2.0));
    must(integral.set_rule(SiteId::send, b));

    assert(fractional.validate(kNodes).has_value());
    assert(integral.validate(kNodes).has_value());

    fractional.normalize();
    integral.normalize();

    const cosmos::OutcomeTable& left = fractional.rule_for(SiteId::send)->outcomes;
    const cosmos::OutcomeTable& right = integral.rule_for(SiteId::send)->outcomes;
    assert(left.count == 3);
    assert(right.count == 3);

    double total = 0.0;
    for (size_t i = 0; i < left.count; ++i) {
        assert(left.entries[i].kind == right.entries[i].kind);
        assert(std::fabs(left.entries[i].weight - right.entries[i].weight) < 1e-12);
        total += left.entries[i].weight;
    }
    assert(std::fabs(total - 1.0) < 1e-12);

    // Normalizing an already-normalized table must not drift it.
    fractional.normalize();
    for (size_t i = 0; i < left.count; ++i) {
        assert(std::fabs(left.entries[i].weight - right.entries[i].weight) < 1e-12);
    }

    std::cout << "[PASS] test_weight_normalization_is_scale_invariant" << std::endl;
}

void test_slot_mapping_round_trips() {
    for (size_t slot = 0; slot < cosmos::kSiteCount; ++slot) {
        assert(cosmos::site_slot(cosmos::site_at_slot(slot)) == slot);
    }

    FaultConfig cfg;
    assert(cfg.rule_for(SiteId::malloc) == nullptr);
    must(cfg.set_rule(SiteId::malloc, oom_rule(0.25)));
    assert(cfg.rule_for(SiteId::malloc) != nullptr);
    assert(cfg.rule_for(SiteId::malloc)->rate == 0.25);
    assert(cfg.rule_for(SiteId::send) == nullptr);

    assert(!cfg.is_site_activated(SiteId::malloc));
    must(cfg.activate_site(SiteId::malloc));
    assert(cfg.is_site_activated(SiteId::malloc));
    assert(!cfg.is_site_activated(SiteId::send));

    std::cout << "[PASS] test_slot_mapping_round_trips" << std::endl;
}

// §2: the limits are the fault model. A config that may crash N while demanding M healthy, with
// N + M past the cluster size, cannot honour both.
void test_node_limits_must_be_satisfiable() {
    FaultConfig contradictory = valid_config();
    contradictory.max_crashed_nodes = 3;
    contradictory.min_healthy_quorum = 2;
    assert_rejects(contradictory, ConfigError::LimitsExceedNodes);

    FaultConfig too_many = valid_config();
    too_many.max_crashed_nodes = 99;
    assert_rejects(too_many, ConfigError::QuorumExceedsNodes);

    FaultConfig exact = valid_config();
    exact.max_crashed_nodes = 1;
    exact.min_healthy_quorum = 2;
    assert(exact.validate(kNodes).has_value());

    std::cout << "[PASS] test_node_limits_must_be_satisfiable" << std::endl;
}

void test_episodes_reference_real_nodes() {
    FaultConfig crash = valid_config();
    crash.scheduled_episodes.clear();
    crash.scheduled_episodes.push_back({cosmos::Time::zero() + 30_ms, cosmos::CrashNode{99}, 2_s});
    assert_rejects(crash, ConfigError::UnknownNode);

    FaultConfig pause = valid_config();
    pause.scheduled_episodes.clear();
    pause.scheduled_episodes.push_back({cosmos::Time::zero() + 30_ms, cosmos::PauseNode{3}, 2_s});
    assert_rejects(pause, ConfigError::UnknownNode);

    FaultConfig split = valid_config();
    split.scheduled_episodes.clear();
    split.scheduled_episodes.push_back(
        {cosmos::Time::zero() + 30_ms, cosmos::Partition{{0}, {1, 7}}, 2_s});
    assert_rejects(split, ConfigError::UnknownNode);

    FaultConfig ok = valid_config();
    ok.scheduled_episodes.clear();
    ok.scheduled_episodes.push_back(
        {cosmos::Time::zero() + 30_ms, cosmos::Partition{{0}, {1, 2}}, 2_s});
    assert(ok.validate(kNodes).has_value());

    std::cout << "[PASS] test_episodes_reference_real_nodes" << std::endl;
}

// Rule 5: an episode must schedule its own heal, and a zero-length one heals the instant it
// starts, so the application never observes it.
void test_episodes_must_have_a_duration() {
    FaultConfig zero = valid_config();
    zero.scheduled_episodes.clear();
    zero.scheduled_episodes.push_back(
        {cosmos::Time::zero() + 30_ms, cosmos::CrashNode{1}, cosmos::Duration::zero()});
    assert_rejects(zero, ConfigError::BadEpisodeDuration);

    FaultConfig negative = valid_config();
    negative.scheduled_episodes.clear();
    negative.scheduled_episodes.push_back(
        {cosmos::Time::zero() + 30_ms, cosmos::CrashNode{1}, -(1_s)});
    assert_rejects(negative, ConfigError::BadEpisodeDuration);

    std::cout << "[PASS] test_episodes_must_have_a_duration" << std::endl;
}

// Rule 15 at config time: an outcome the site's own API could never produce tests the application
// against an impossible world, and any finding from it is a false positive.
void test_outcomes_must_be_legal_for_their_site() {
    FaultConfig borrowed = valid_config();
    FaultRule rule;
    rule.rate = 0.5;
    must(rule.outcomes.add(FaultKind::ConnReset, 1.0));
    must(borrowed.set_rule(SiteId::malloc, rule));
    assert_rejects(borrowed, ConfigError::IllegalOutcome);
    assert(borrowed.validate(kNodes).error().site == SiteId::malloc);

    // getrandom has its own kind now; another subsystem's must not stand in for it.
    FaultConfig random_site = valid_config();
    random_site.enable_class(FaultClass::Random);
    must(random_site.activate_site(SiteId::getrandom));
    FaultRule wrong;
    wrong.rate = 0.5;
    must(wrong.outcomes.add(FaultKind::ReadEio, 1.0));
    must(random_site.set_rule(SiteId::getrandom, wrong));
    assert_rejects(random_site, ConfigError::IllegalOutcome);

    FaultConfig right = valid_config();
    right.enable_class(FaultClass::Random);
    must(right.activate_site(SiteId::getrandom));
    FaultRule legal;
    legal.rate = 0.5;
    must(legal.outcomes.add(FaultKind::RandomEagain, 1.0));
    must(right.set_rule(SiteId::getrandom, legal));
    assert(right.validate(kNodes).has_value());

    // None means "pass through", so it is never a table entry.
    FaultConfig none_kind = valid_config();
    FaultRule passthrough;
    passthrough.rate = 0.5;
    must(passthrough.outcomes.add(FaultKind::None, 1.0));
    must(none_kind.set_rule(SiteId::malloc, passthrough));
    assert_rejects(none_kind, ConfigError::BadOutcomeKind);

    std::cout << "[PASS] test_outcomes_must_be_legal_for_their_site" << std::endl;
}

// A setter that quietly does nothing on an unrecognised site costs a debugging session, and P5
// replay reads site ids out of a recorded trace.
void test_setters_report_unknown_sites() {
    FaultConfig cfg;
    const SiteId unknown = static_cast<SiteId>(9999);
    assert(!cfg.set_rule(unknown, FaultRule{}));
    assert(!cfg.activate_site(unknown));
    assert(!cfg.is_site_activated(unknown));
    assert(cfg.rule_for(unknown) == nullptr);
    assert(cfg.validate(kNodes).has_value());

    std::cout << "[PASS] test_setters_report_unknown_sites" << std::endl;
}

void test_outcome_table_reports_overflow() {
    cosmos::OutcomeTable table;
    for (size_t i = 0; i < cosmos::kMaxOutcomes; ++i) {
        assert(table.add(FaultKind::PacketDrop, 1.0));
    }
    assert(table.full());
    assert(!table.add(FaultKind::PacketDelay, 1.0));
    assert(table.count == cosmos::kMaxOutcomes);

    std::cout << "[PASS] test_outcome_table_reports_overflow" << std::endl;
}

// The table must hold every outcome the site is allowed to produce, or a config asking for send's
// full menu silently becomes a different config.
void test_widest_site_menu_fits_in_a_table() {
    FaultConfig cfg = valid_config();
    FaultRule rule;
    rule.rate = 0.5;
    for (FaultKind kind :
         {FaultKind::ConnReset, FaultKind::ShortSend, FaultKind::PacketDrop, FaultKind::PacketDelay,
          FaultKind::PacketReorder, FaultKind::PacketCorrupt}) {
        assert(cosmos::is_legal_outcome(SiteId::send, kind));
        must(rule.outcomes.add(kind, 1.0));
    }
    assert(rule.outcomes.count == 6);
    must(cfg.set_rule(SiteId::send, rule));
    assert(cfg.validate(kNodes).has_value());

    cfg.normalize();
    double total = 0.0;
    for (const cosmos::SiteOutcome& outcome : cfg.rule_for(SiteId::send)->outcomes) {
        total += outcome.weight;
    }
    assert(std::fabs(total - 1.0) < 1e-12);

    std::cout << "[PASS] test_widest_site_menu_fits_in_a_table" << std::endl;
}

// The doc calls knobs "sorted by id"; a comment cannot enforce the reproduction contract, so
// validate() does.
void test_knobs_must_be_ordered() {
    FaultConfig descending = valid_config();
    descending.knobs.clear();
    descending.knobs.push_back({7, 1});
    descending.knobs.push_back({3, 2});
    assert_rejects(descending, ConfigError::BadKnobOrder);

    FaultConfig duplicate = valid_config();
    duplicate.knobs.clear();
    duplicate.knobs.push_back({3, 1});
    duplicate.knobs.push_back({3, 2});
    assert_rejects(duplicate, ConfigError::BadKnobOrder);

    FaultConfig ordered = valid_config();
    ordered.knobs.clear();
    ordered.knobs.push_back({1, 10});
    ordered.knobs.push_back({2, 20});
    ordered.knobs.push_back({9, 30});
    assert(ordered.validate(kNodes).has_value());

    std::cout << "[PASS] test_knobs_must_be_ordered" << std::endl;
}

int main() {
    test_valid_config_passes();
    test_bad_rates_are_rejected();
    test_bad_outcome_tables_are_rejected();
    test_trigger_must_exceed_skip_first();
    test_event_sites_reject_rules_and_triggers();
    test_rule_on_disabled_class_is_rejected();
    test_windows_and_quorum_are_checked();
    test_weight_normalization_is_scale_invariant();
    test_slot_mapping_round_trips();
    test_node_limits_must_be_satisfiable();
    test_episodes_reference_real_nodes();
    test_episodes_must_have_a_duration();
    test_outcomes_must_be_legal_for_their_site();
    test_setters_report_unknown_sites();
    test_outcome_table_reports_overflow();
    test_widest_site_menu_fits_in_a_table();
    test_knobs_must_be_ordered();
    std::cout << "All fault config tests passed successfully!" << std::endl;
    return 0;
}
