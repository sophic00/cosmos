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
using cosmos::Rng;
using cosmos::SiteId;
using cosmos::VirtualClock;

namespace {

constexpr uint32_t kNodes = 3;
constexpr uint64_t kSeed = 0x9E3779B97F4A7C15ULL;
constexpr uint64_t kDecisions = 1'000'000;
constexpr uint64_t kWarmupDecisions = 50'000;

// Each probe decision reveals one bit of the value behind it, so a stream sitting at the wrong
// position survives a single comparison half the time. Sixty-four of them make that 2^-64.
constexpr int kProbeCalls = 64;

using Injector = cosmos::BasicFaultInjector<VirtualClock>;
using Guard = cosmos::QuietGuard<Injector>;

void must(bool ok) { assert(ok); }

FaultRule rule_of(double rate, std::initializer_list<FaultKind> kinds) {
    FaultRule rule;
    rule.rate = rate;
    for (FaultKind kind : kinds) {
        must(rule.outcomes.add(kind, 1.0));
    }
    return rule;
}

// Every site here exists to make one gate observable at scale. The three "normal" sites draw on
// every eligible call; the rest must never reach the draw, each for a different reason.
FaultConfig gate_config() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Memory);
    cfg.enable_class(FaultClass::Storage);
    cfg.enable_class(FaultClass::Network);
    // Clock stays disabled: clock_gettime is the class-off case.

    cfg.warmup_until = cosmos::Time::zero() + 100_ms;
    cfg.quiesce_after = cosmos::Time::zero() + 10_s;

    must(cfg.activate_site(SiteId::malloc));
    must(cfg.set_rule(SiteId::malloc, rule_of(0.5, {FaultKind::OutOfMemory})));
    must(cfg.activate_site(SiteId::write));
    must(cfg.set_rule(SiteId::write, rule_of(0.3, {FaultKind::WriteEio, FaultKind::ShortWrite})));
    must(cfg.activate_site(SiteId::send));
    must(cfg.set_rule(SiteId::send, rule_of(0.7, {FaultKind::PacketDrop, FaultKind::ConnReset})));

    // Budget spent from the first call: eligible, never drawing.
    FaultRule spent = rule_of(1.0, {FaultKind::PeerClose});
    spent.max_injections = 0;
    must(cfg.activate_site(SiteId::recv));
    must(cfg.set_rule(SiteId::recv, std::move(spent)));

    // skip_first past any reachable count: eligible, never drawing.
    FaultRule skipped = rule_of(1.0, {FaultKind::FsyncEio});
    skipped.skip_first = UINT64_MAX;
    must(cfg.activate_site(SiteId::fsync));
    must(cfg.set_rule(SiteId::fsync, std::move(skipped)));

    // A zero-rate trigger: fires once, deterministically, without a draw (Rule 11).
    FaultRule triggered = rule_of(0.0, {FaultKind::ConnReset});
    triggered.fire_on_eligible_call = 1000;
    must(cfg.activate_site(SiteId::accept));
    must(cfg.set_rule(SiteId::accept, std::move(triggered)));

    // Class enabled but site never activated.
    must(cfg.set_rule(SiteId::read, rule_of(1.0, {FaultKind::ReadEio})));

    // Probe sites: untouched during the run, so every draw they consume is one this test asked for.
    must(cfg.activate_site(SiteId::calloc));
    must(cfg.set_rule(SiteId::calloc, rule_of(0.5, {FaultKind::OutOfMemory})));
    must(cfg.activate_site(SiteId::open));
    must(cfg.set_rule(SiteId::open, rule_of(1.0, {FaultKind::OpenEio, FaultKind::NoSpace})));
    must(cfg.activate_site(SiteId::connect));
    must(cfg.set_rule(SiteId::connect,
                      rule_of(1.0, {FaultKind::ConnRefused, FaultKind::ConnReset})));
    return cfg;
}

Injector make_injector(const VirtualClock& clock) {
    auto injector = Injector::create(gate_config(), kSeed, kNodes, clock);
    must(injector.has_value());
    return std::move(*injector);
}

// Counted by construction rather than by re-deriving decide()'s gate logic: a recount that mirrors
// the implementation would agree with it about a shared bug.
struct DrawCount {
    uint64_t memory = 0;
    uint64_t storage = 0;
    uint64_t network = 0;
};

// Every call below is either a site whose rule draws on every eligible call, or a site blocked by
// exactly one gate. Nothing in between, so the expected draw count is the number of the former.
DrawCount run_mixed_decisions(Injector& injector, uint64_t decisions) {
    DrawCount drawn;
    for (uint64_t i = 0; i < decisions; ++i) {
        switch (i % 10) {
        case 0:
        case 1:
            injector.decide(FaultClass::Memory, SiteId::malloc);
            ++drawn.memory;
            break;
        case 2: {
            // Quiet: the engine's own critical section must not consume the stream.
            Guard guard(injector);
            injector.decide(FaultClass::Memory, SiteId::malloc);
            break;
        }
        case 3:
        case 4:
            injector.decide(FaultClass::Storage, SiteId::write);
            ++drawn.storage;
            break;
        case 5:
            injector.decide(FaultClass::Network, SiteId::send);
            ++drawn.network;
            break;
        case 6:
            injector.decide(FaultClass::Network, SiteId::recv); // budget spent
            break;
        case 7:
            injector.decide(FaultClass::Storage, SiteId::fsync); // skip_first
            break;
        case 8:
            injector.decide(FaultClass::Network, SiteId::accept); // zero-rate trigger
            break;
        case 9:
            injector.decide(FaultClass::Storage, SiteId::read);        // site never activated
            injector.decide(FaultClass::Clock, SiteId::clock_gettime); // class disabled
            injector.decide(FaultClass::Process, SiteId::crash_node);  // event site
            break;
        default:
            assert(false);
            break;
        }
    }
    return drawn;
}

// Advances a fresh stream to where the run should have left it, then requires the injector to agree
// for kProbeCalls decisions running. One bit each, so a misplaced stream cannot survive the run.
void expect_stream_at(Injector& injector, FaultClass fault_class, uint64_t consumed, SiteId probe,
                      FaultKind low, FaultKind high, Rng& reference) {
    for (uint64_t i = 0; i < consumed; ++i) {
        (void)reference.uniform();
    }
    for (int i = 0; i < kProbeCalls; ++i) {
        const double value = reference.uniform();
        const FaultKind expected = high == FaultKind::None ? (value < 0.5 ? low : FaultKind::None)
                                                           : (value < 0.5 ? low : high);
        assert(injector.decide(fault_class, probe) == expected);
    }
}

void test_phase1_gate_zero_rng_leakage_over_a_million_decisions() {
    VirtualClock clock;
    Injector injector = make_injector(clock);

    // Warmup: every gate below it is open, so only the warmup gate can be stopping these.
    DrawCount blocked = run_mixed_decisions(injector, kWarmupDecisions);
    assert(blocked.memory > 0 && blocked.storage > 0 && blocked.network > 0);

    clock.advance(200_ms);
    const DrawCount drawn = run_mixed_decisions(injector, kDecisions);

    // Nothing before this point may have consumed a draw except the counted calls.
    Rng memory_stream(cosmos::fault_class_seed(kSeed, FaultClass::Memory));
    Rng storage_stream(cosmos::fault_class_seed(kSeed, FaultClass::Storage));
    Rng network_stream(cosmos::fault_class_seed(kSeed, FaultClass::Network));
    expect_stream_at(injector, FaultClass::Memory, drawn.memory, SiteId::calloc,
                     FaultKind::OutOfMemory, FaultKind::None, memory_stream);
    expect_stream_at(injector, FaultClass::Storage, drawn.storage, SiteId::open, FaultKind::OpenEio,
                     FaultKind::NoSpace, storage_stream);
    expect_stream_at(injector, FaultClass::Network, drawn.network, SiteId::connect,
                     FaultKind::ConnRefused, FaultKind::ConnReset, network_stream);

    // The trigger fired exactly once and spent no draw; the probes above already proved the second
    // half, since a leaked trigger draw would have shifted the Network stream.
    assert(injector.injections(SiteId::accept) == 1);
    assert(injector.injections(SiteId::recv) == 0);
    assert(injector.injections(SiteId::fsync) == 0);
    assert(injector.eligible_calls(SiteId::read) == 0);
    assert(injector.eligible_calls(SiteId::clock_gettime) == 0);
    assert(injector.eligible_calls(SiteId::crash_node) == 0);
    assert(injector.eligible_calls(SiteId::recv) > 0);
    assert(injector.eligible_calls(SiteId::fsync) > 0);

    // Quiesce closes the window; the streams must be exactly where the probes left them.
    clock.advance(20_s);
    run_mixed_decisions(injector, 1000);

    // Back inside the window to read the streams: a probe run while quiesced would be gated off and
    // could not observe anything. The injector only reads now(), so moving it costs no other state.
    clock.set(cosmos::Time::zero() + 200_ms);
    expect_stream_at(injector, FaultClass::Memory, 0, SiteId::calloc, FaultKind::OutOfMemory,
                     FaultKind::None, memory_stream);
    expect_stream_at(injector, FaultClass::Storage, 0, SiteId::open, FaultKind::OpenEio,
                     FaultKind::NoSpace, storage_stream);
    expect_stream_at(injector, FaultClass::Network, 0, SiteId::connect, FaultKind::ConnRefused,
                     FaultKind::ConnReset, network_stream);

    std::cout << "[PASS] test_phase1_gate_zero_rng_leakage_over_a_million_decisions"
              << " (memory=" << drawn.memory << " storage=" << drawn.storage
              << " network=" << drawn.network << " draws)" << std::endl;
}

void test_phase1_gate_same_seed_is_reproducible() {
    VirtualClock left_clock;
    VirtualClock right_clock;
    Injector left = make_injector(left_clock);
    Injector right = make_injector(right_clock);
    left_clock.advance(200_ms);
    right_clock.advance(200_ms);

    for (uint64_t i = 0; i < 100'000; ++i) {
        const SiteId site = i % 3 == 0 ? SiteId::malloc : i % 3 == 1 ? SiteId::write : SiteId::send;
        const FaultClass fault_class = cosmos::class_of(site);
        assert(left.decide(fault_class, site) == right.decide(fault_class, site));
    }

    std::ostringstream left_out;
    std::ostringstream right_out;
    cosmos::print_ledger(left_out, left);
    cosmos::print_ledger(right_out, right);
    assert(left_out.str() == right_out.str());
    assert(left_out.str().find("fired=") != std::string::npos);

    std::cout << "[PASS] test_phase1_gate_same_seed_is_reproducible" << std::endl;
}

} // namespace

int main() {
    test_phase1_gate_zero_rng_leakage_over_a_million_decisions();
    test_phase1_gate_same_seed_is_reproducible();
    std::cout << "Phase 1 exit gate passed." << std::endl;
    return 0;
}
