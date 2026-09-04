#include "cosmos/fault_injector.hpp"
#include "cosmos/ledger_print.hpp"
#include "cosmos/virtual_clock.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

using namespace cosmos::literals;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultKind;
using cosmos::FaultRule;
using cosmos::LedgerEntry;
using cosmos::SiteId;
using cosmos::VirtualClock;

namespace {

constexpr uint32_t kNodes = 3;
constexpr uint64_t kSeed = 0x5CA1AB1E;

using Injector = cosmos::BasicFaultInjector<VirtualClock>;

void must(bool ok) { assert(ok); }

FaultRule rule_with(double rate, FaultKind kind) {
    FaultRule rule;
    rule.rate = rate;
    must(rule.outcomes.add(kind, 1.0));
    return rule;
}

Injector make_injector(FaultConfig cfg, const VirtualClock& clock) {
    auto injector = Injector::create(std::move(cfg), kSeed, kNodes, clock);
    must(injector.has_value());
    return std::move(*injector);
}

FaultConfig one_site(SiteId site, FaultRule rule) {
    FaultConfig cfg;
    cfg.enable_class(cosmos::class_of(site));
    must(cfg.activate_site(site));
    must(cfg.set_rule(site, std::move(rule)));
    return cfg;
}

// Both fires share one rule, so drew is the only thing that can distinguish them.
FaultConfig mixed_config() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Storage);
    must(cfg.activate_site(SiteId::write));
    must(cfg.activate_site(SiteId::open));
    must(cfg.set_rule(SiteId::write, rule_with(1.0, FaultKind::WriteEio)));

    FaultRule triggered = rule_with(0.0, FaultKind::OpenEio);
    triggered.fire_on_eligible_call = 2;
    must(cfg.set_rule(SiteId::open, triggered));
    return cfg;
}

std::string printed(const Injector& injector) {
    std::ostringstream out;
    cosmos::print_ledger(out, injector);
    return out.str();
}

void test_fired_entry_records_every_field() {
    VirtualClock clock;
    clock.advance(12_ms);
    Injector injector =
        make_injector(one_site(SiteId::write, rule_with(1.0, FaultKind::WriteEio)), clock);

    assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::WriteEio);
    assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::WriteEio);
    assert(injector.ledger().size() == 2);

    const LedgerEntry& first = injector.ledger()[0];
    assert(first.fire_index == 0);
    assert(first.at == cosmos::Time::zero() + 12_ms);
    assert(first.fault_class == FaultClass::Storage);
    assert(first.site == SiteId::write);
    assert(first.outcome == FaultKind::WriteEio);
    assert(first.eligible_index == 1);
    assert(first.drew);

    assert(injector.ledger()[1].fire_index == 1);
    assert(injector.ledger()[1].eligible_index == 2);

    std::cout << "[PASS] test_fired_entry_records_every_field" << std::endl;
}

// Closes P1-S4 checkpoint 2's ledger half: a scripted fire is distinguishable from a rolled one.
void test_drew_flag_separates_scripted_from_probabilistic() {
    VirtualClock clock;
    Injector injector = make_injector(mixed_config(), clock);

    assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::WriteEio);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::OpenEio);

    assert(injector.ledger().size() == 2);
    assert(injector.ledger()[0].site == SiteId::write);
    assert(injector.ledger()[0].drew);
    assert(injector.ledger()[1].site == SiteId::open);
    assert(!injector.ledger()[1].drew);

    std::cout << "[PASS] test_drew_flag_separates_scripted_from_probabilistic" << std::endl;
}

// §11.1 lists a spent budget among the routine gate rejections, which are counted and never
// recorded. Replaces P1-S4 checkpoint 4's struck "ledger shows reason budget_spent" half.
void test_budget_blocked_trigger_records_nothing() {
    VirtualClock clock;
    FaultRule rule = rule_with(0.0, FaultKind::OpenEio);
    rule.fire_on_eligible_call = 1;
    rule.max_injections = 0;
    Injector injector = make_injector(one_site(SiteId::open, std::move(rule)), clock);

    assert(injector.decide(FaultClass::Storage, SiteId::open) == FaultKind::None);
    assert(injector.ledger().size() == 0);
    assert(injector.ledger().dropped() == 0);
    assert(injector.eligible_calls(SiteId::open) == 1);
    assert(injector.injections(SiteId::open) == 0);

    std::cout << "[PASS] test_budget_blocked_trigger_records_nothing" << std::endl;
}

// The "routine rejects are counted, not recorded" guarantee, at the scale that motivates it.
void test_a_million_no_fire_calls_record_nothing() {
    constexpr uint64_t kCalls = 1'000'000;

    VirtualClock clock;
    Injector injector =
        make_injector(one_site(SiteId::write, rule_with(0.0, FaultKind::WriteEio)), clock);

    for (uint64_t call = 0; call < kCalls; ++call) {
        assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::None);
    }

    assert(injector.ledger().size() == 0);
    assert(injector.ledger().dropped() == 0);
    assert(injector.eligible_calls(SiteId::write) == kCalls);
    assert(injector.injections(SiteId::write) == 0);

    std::cout << "[PASS] test_a_million_no_fire_calls_record_nothing" << std::endl;
}

// The interesting case sits between the rate-0 and rate-1 tests: a run that both fires and does
// not must still have exactly one entry per fire.
void test_partial_rate_run_records_one_entry_per_fire() {
    // Kept under kLedgerCapacity so a full ledger cannot stand in for a correct one.
    constexpr uint64_t kCalls = 300;

    VirtualClock clock;
    Injector injector =
        make_injector(one_site(SiteId::write, rule_with(0.5, FaultKind::WriteEio)), clock);

    uint64_t fires = 0;
    for (uint64_t call = 0; call < kCalls; ++call) {
        if (injector.decide(FaultClass::Storage, SiteId::write) != FaultKind::None) ++fires;
    }

    assert(fires > 0 && fires < kCalls);
    assert(fires < cosmos::kLedgerCapacity);
    assert(injector.ledger().size() == fires);
    assert(injector.ledger().dropped() == 0);
    assert(injector.injections(SiteId::write) == fires);
    assert(injector.eligible_calls(SiteId::write) == kCalls);

    std::cout << "[PASS] test_partial_rate_run_records_one_entry_per_fire" << std::endl;
}

void test_ledger_overflow_is_counted_and_counters_stay_exact() {
    constexpr uint64_t kExtra = 5;
    const uint64_t calls = cosmos::kLedgerCapacity + kExtra;

    VirtualClock clock;
    Injector injector =
        make_injector(one_site(SiteId::write, rule_with(1.0, FaultKind::WriteEio)), clock);

    for (uint64_t call = 0; call < calls; ++call) {
        assert(injector.decide(FaultClass::Storage, SiteId::write) == FaultKind::WriteEio);
    }

    assert(injector.ledger().size() == cosmos::kLedgerCapacity);
    assert(injector.ledger().dropped() == kExtra);
    // The counters are what §11.4's coverage checks read, so they must survive an overflow intact.
    assert(injector.injections(SiteId::write) == calls);
    assert(printed(injector).find("and 5 more not recorded") != std::string::npos);

    std::cout << "[PASS] test_ledger_overflow_is_counted_and_counters_stay_exact" << std::endl;
}

void test_printed_ledger_matches_the_doc_format() {
    VirtualClock clock;
    Injector injector = make_injector(mixed_config(), clock);

    injector.decide(FaultClass::Storage, SiteId::write);
    injector.decide(FaultClass::Storage, SiteId::open);
    injector.decide(FaultClass::Storage, SiteId::open);

    const std::string expected =
        "Fault ledger:\n"
        "  t=0ms    Storage  site=write           FIRED    drew=yes  eligible #1 -> WriteEio\n"
        "  t=0ms    Storage  site=open            FIRED    drew=no   eligible #2 -> OpenEio\n"
        "\n"
        "Per-site counters:  open eligible=2 fired=1 · write eligible=1 fired=1\n";
    assert(printed(injector) == expected);

    std::cout << "[PASS] test_printed_ledger_matches_the_doc_format" << std::endl;
}

// The t=0ms snapshot above cannot tell auto-sizing from a fixed width, nor microsecond formatting
// from truncation: at that width and value the old and new printers agree byte for byte.
void test_printed_times_keep_resolution_and_alignment() {
    VirtualClock clock;
    Injector injector =
        make_injector(one_site(SiteId::write, rule_with(1.0, FaultKind::WriteEio)), clock);

    injector.decide(FaultClass::Storage, SiteId::write);
    clock.advance(1_ms + 500_us);
    injector.decide(FaultClass::Storage, SiteId::write);
    clock.advance(29_s);
    injector.decide(FaultClass::Storage, SiteId::write);

    const std::string expected = "Fault ledger:\n"
                                 "  t=0ms          Storage  site=write           FIRED    drew=yes "
                                 " eligible #1 -> WriteEio\n"
                                 "  t=1.500ms      Storage  site=write           FIRED    drew=yes "
                                 " eligible #2 -> WriteEio\n"
                                 "  t=29001.500ms  Storage  site=write           FIRED    drew=yes "
                                 " eligible #3 -> WriteEio\n"
                                 "\n"
                                 "Per-site counters:  write eligible=3 fired=3\n";
    assert(printed(injector) == expected);

    std::cout << "[PASS] test_printed_times_keep_resolution_and_alignment" << std::endl;
}

// A negative timestamp is unreachable while warmup_until defaults to zero, but the printer must not
// throw while reporting a failure if that ever changes.
void test_negative_timestamp_formats_without_throwing() {
    assert(cosmos::detail::format_time(cosmos::Time{-1'500'000}) == "t=-1.500ms");
    assert(cosmos::detail::format_time(cosmos::Time{-500}) == "t=-0ms");
    assert(cosmos::detail::format_time(cosmos::Time::min()).size() > 3);
    assert(cosmos::detail::format_time(cosmos::Time{999'000}) == "t=0.999ms");

    std::cout << "[PASS] test_negative_timestamp_formats_without_throwing" << std::endl;
}

void test_empty_ledger_prints_a_placeholder() {
    VirtualClock clock;
    Injector injector =
        make_injector(one_site(SiteId::write, rule_with(0.0, FaultKind::WriteEio)), clock);

    const std::string expected = "Fault ledger:\n"
                                 "  (no faults fired)\n"
                                 "\n"
                                 "Per-site counters:  (none exercised)\n";
    assert(printed(injector) == expected);

    std::cout << "[PASS] test_empty_ledger_prints_a_placeholder" << std::endl;
}

void test_same_seed_prints_identical_ledgers() {
    VirtualClock left_clock;
    VirtualClock right_clock;
    Injector left = make_injector(mixed_config(), left_clock);
    Injector right = make_injector(mixed_config(), right_clock);

    for (int i = 0; i < 500; ++i) {
        const SiteId site = i % 3 == 0 ? SiteId::open : SiteId::write;
        assert(left.decide(FaultClass::Storage, site) == right.decide(FaultClass::Storage, site));
    }

    assert(printed(left) == printed(right));
    assert(printed(left).find("fired=") != std::string::npos);

    std::cout << "[PASS] test_same_seed_prints_identical_ledgers" << std::endl;
}

// Thousands grouping is done by hand so the output cannot vary with an ambient locale.
void test_counter_grouping_is_locale_independent() {
    assert(cosmos::detail::grouped(0) == "0");
    assert(cosmos::detail::grouped(999) == "999");
    assert(cosmos::detail::grouped(1000) == "1,000");
    assert(cosmos::detail::grouped(12004) == "12,004");
    assert(cosmos::detail::grouped(1000000) == "1,000,000");

    std::cout << "[PASS] test_counter_grouping_is_locale_independent" << std::endl;
}

} // namespace

int main() {
    test_fired_entry_records_every_field();
    test_drew_flag_separates_scripted_from_probabilistic();
    test_budget_blocked_trigger_records_nothing();
    test_a_million_no_fire_calls_record_nothing();
    test_partial_rate_run_records_one_entry_per_fire();
    test_ledger_overflow_is_counted_and_counters_stay_exact();
    test_printed_ledger_matches_the_doc_format();
    test_printed_times_keep_resolution_and_alignment();
    test_negative_timestamp_formats_without_throwing();
    test_empty_ledger_prints_a_placeholder();
    test_same_seed_prints_identical_ledgers();
    test_counter_grouping_is_locale_independent();
    std::cout << "All ledger tests passed successfully!" << std::endl;
    return 0;
}
