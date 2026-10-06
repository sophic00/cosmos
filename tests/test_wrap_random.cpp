#include "cosmos/cosmos.hpp"
#include "wrapper_fault.hpp"
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/random.h>

namespace {

constexpr uint64_t kSeedA = 0x12345678ABCDEF01ULL;
constexpr uint64_t kSeedB = 0xFEDCBA9876543210ULL;

void must(bool ok) { assert(ok); }

cosmos::FaultConfig eagain_config(double rate) {
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Random);
    must(cfg.activate_site(cosmos::SiteId::getrandom));
    cosmos::FaultRule rule;
    rule.rate = rate;
    must(rule.outcomes.add(cosmos::FaultKind::RandomEagain, 1.0));
    must(cfg.set_rule(cosmos::SiteId::getrandom, rule));
    return cfg;
}

void fill_reference(cosmos::Rng& rng, unsigned char* out, size_t len) {
    size_t remaining = len;
    while (remaining > 0) {
        const uint64_t word = rng.next();
        const size_t chunk = remaining < sizeof(word) ? remaining : sizeof(word);
        memcpy(out + (len - remaining), &word, chunk);
        remaining -= chunk;
    }
}

} // namespace

void test_passthrough_no_sim() {
    assert(!cosmos::Simulator::has_current());

    unsigned char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t n = getrandom(buf, sizeof(buf), 0);
    assert(n == (ssize_t)sizeof(buf));

    // The host kernel owns validation outside a universe: zero-length succeeds, unknown flag
    // bits fail with EINVAL even though this wrapper could have answered 0 itself.
    errno = 0;
    assert(getrandom(buf, 0, 0) == 0);
    errno = 0;
    assert(getrandom(buf, sizeof(buf), 0x80000000u) == -1);
    assert(errno == EINVAL);

    long r = random();
    assert(r >= 0 && r <= RAND_MAX);
    int ir = rand();
    assert(ir >= 0 && ir <= RAND_MAX);

    // Outside a universe, seeding reaches the host RNG like every other passthrough call: the
    // same seed must reproduce the same next draw. (Inside a universe seeding is a no-op; that
    // half of the contract is pinned by test_srandom_srand_are_noops.)
    srand(7);
    const int seeded_a = rand();
    srand(7);
    const int seeded_b = rand();
    assert(seeded_a == seeded_b);
    srandom(8);
    const long seeded_c = random();
    srandom(8);
    const long seeded_d = random();
    assert(seeded_c == seeded_d);

    std::cout << "[PASS] test_passthrough_no_sim" << std::endl;
}

void test_getrandom_arg_errors() {
    cosmos::Simulator sim(kSeedA);
    cosmos::Simulator::set_current(&sim);

    unsigned char buf[16];

    // Kernel validation order (drivers/char/random.c): flags, then count, then buffer.
    // Unknown flag bits fail before the count and before the buffer are considered.
    errno = 0;
    assert(getrandom(buf, sizeof(buf), 0x80000000u) == -1);
    assert(errno == EINVAL);

    errno = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull"
    // Intentional NULL: flags must be rejected before the buffer is examined.
    const bool flags_before_buf = (getrandom(nullptr, 16, 0x80000000u) == -1);
#pragma GCC diagnostic pop
    assert(flags_before_buf);
    assert(errno == EINVAL);

    // Flags are also rejected before the count == 0 short-circuit.
    errno = 0;
    assert(getrandom(buf, 0, 0x80000000u) == -1);
    assert(errno == EINVAL);

#if defined(GRND_INSECURE) && defined(GRND_RANDOM)
    // GRND_INSECURE and GRND_RANDOM are mutually exclusive per the kernel.
    errno = 0;
    assert(getrandom(buf, sizeof(buf), GRND_INSECURE | GRND_RANDOM) == -1);
    assert(errno == EINVAL);
#endif

    errno = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull"
    // Intentional NULL to exercise the EFAULT path.
    const bool null_rejected = (getrandom(nullptr, 16, 0) == -1);
#pragma GCC diagnostic pop
    assert(null_rejected);
    assert(errno == EFAULT);

    // A zero-length read with valid flags succeeds and never touches the buffer.
    errno = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull"
    const bool zero_len_ok = (getrandom(nullptr, 0, 0) == 0);
#pragma GCC diagnostic pop
    assert(zero_len_ok);
    assert(errno == 0);

    // Error paths must not advance the User stream: the next successful call matches a fresh sim.
    cosmos::Rng expected(cosmos::stream_seed(kSeedA, cosmos::StreamDomain::User));
    unsigned char want[16];
    unsigned char got[16];
    fill_reference(expected, want, sizeof(want));
    assert(getrandom(got, sizeof(got), 0) == (ssize_t)sizeof(got));
    assert(memcmp(want, got, sizeof(want)) == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_getrandom_arg_errors" << std::endl;
}

void test_getrandom_flags_accepted() {
    unsigned flags[] = {
        0,
#ifdef GRND_NONBLOCK
        GRND_NONBLOCK,
#endif
#ifdef GRND_RANDOM
        GRND_RANDOM,
#endif
#ifdef GRND_INSECURE
        GRND_INSECURE,
#endif
    };
    for (unsigned f : flags) {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        unsigned char buf[16];
        errno = 0;
        ssize_t n = getrandom(buf, sizeof(buf), f);
        assert(n == (ssize_t)sizeof(buf));
        cosmos::Simulator::set_current(nullptr);
    }
    std::cout << "[PASS] test_getrandom_flags_accepted" << std::endl;
}

void test_getrandom_determinism_same_seed() {
    constexpr size_t kLens[] = {1, 7, 8, 9, 16, 31, 32, 100};
    for (size_t len : kLens) {
        unsigned char first[100];
        unsigned char second[100];
        {
            cosmos::Simulator sim(kSeedA);
            cosmos::Simulator::set_current(&sim);
            assert(getrandom(first, len, 0) == (ssize_t)len);
            cosmos::Simulator::set_current(nullptr);
        }
        {
            cosmos::Simulator sim(kSeedA);
            cosmos::Simulator::set_current(&sim);
            assert(getrandom(second, len, 0) == (ssize_t)len);
            cosmos::Simulator::set_current(nullptr);
        }
        assert(memcmp(first, second, len) == 0);
    }

    // Multi-call sequence replays too.
    unsigned char seq1[64];
    unsigned char seq2[64];
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        for (int i = 0; i < 4; ++i) {
            assert(getrandom(seq1 + i * 16, 16, 0) == 16);
        }
        cosmos::Simulator::set_current(nullptr);
    }
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        for (int i = 0; i < 4; ++i) {
            assert(getrandom(seq2 + i * 16, 16, 0) == 16);
        }
        cosmos::Simulator::set_current(nullptr);
    }
    assert(memcmp(seq1, seq2, sizeof(seq1)) == 0);

    std::cout << "[PASS] test_getrandom_determinism_same_seed" << std::endl;
}

void test_getrandom_matches_user_stream() {
    cosmos::Rng expected(cosmos::stream_seed(kSeedA, cosmos::StreamDomain::User));
    unsigned char want[48];
    fill_reference(expected, want, sizeof(want));

    unsigned char got[48];
    cosmos::Simulator sim(kSeedA);
    cosmos::Simulator::set_current(&sim);
    assert(getrandom(got, sizeof(got), 0) == (ssize_t)sizeof(got));
    cosmos::Simulator::set_current(nullptr);

    assert(memcmp(want, got, sizeof(want)) == 0);
    std::cout << "[PASS] test_getrandom_matches_user_stream" << std::endl;
}

void test_getrandom_different_seed_differs() {
    unsigned char a[32];
    unsigned char b[32];
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        assert(getrandom(a, sizeof(a), 0) == (ssize_t)sizeof(a));
        cosmos::Simulator::set_current(nullptr);
    }
    {
        cosmos::Simulator sim(kSeedB);
        cosmos::Simulator::set_current(&sim);
        assert(getrandom(b, sizeof(b), 0) == (ssize_t)sizeof(b));
        cosmos::Simulator::set_current(nullptr);
    }
    assert(memcmp(a, b, sizeof(a)) != 0);
    std::cout << "[PASS] test_getrandom_different_seed_differs" << std::endl;
}

void test_random_range_and_replay() {
    long seq1[256];
    long seq2[256];
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        for (long& v : seq1) {
            v = random();
            assert(v >= 0 && v <= RAND_MAX);
        }
        cosmos::Simulator::set_current(nullptr);
    }
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        for (long& v : seq2) {
            v = random();
        }
        cosmos::Simulator::set_current(nullptr);
    }
    assert(memcmp(seq1, seq2, sizeof(seq1)) == 0);

    // Cross-check first value against the User stream directly.
    cosmos::Rng expected(cosmos::stream_seed(kSeedA, cosmos::StreamDomain::User));
    const long want = static_cast<long>(expected.range(0, static_cast<uint64_t>(RAND_MAX)));
    assert(seq1[0] == want);

    std::cout << "[PASS] test_random_range_and_replay" << std::endl;
}

// Rule 1: the failure decision and the returned bytes come from different streams.
void test_user_stream_isolated_from_fault_stream() {
    unsigned char baseline[32];
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        assert(getrandom(baseline, sizeof(baseline), 0) == (ssize_t)sizeof(baseline));
        cosmos::Simulator::set_current(nullptr);
    }
    {
        cosmos::Simulator sim(kSeedA);
        must(sim.install_faults(eagain_config(0.5)).has_value());
        cosmos::Simulator::set_current(&sim);
        for (int i = 0; i < 1000; ++i) {
            (void)sim.injector_or_null()->decide(cosmos::FaultClass::Random,
                                                 cosmos::SiteId::getrandom);
        }
        unsigned char got[32];
        assert(getrandom(got, sizeof(got), 0) == (ssize_t)sizeof(got));
        cosmos::Simulator::set_current(nullptr);
        assert(memcmp(baseline, got, sizeof(baseline)) == 0);
    }
    std::cout << "[PASS] test_user_stream_isolated_from_fault_stream" << std::endl;
}

// A failed getrandom must not consume bytes it never delivered, or the next call skips them.
void test_eagain_does_not_consume_the_user_stream() {
    unsigned char baseline[32];
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        assert(getrandom(baseline, sizeof(baseline), 0) == (ssize_t)sizeof(baseline));
        cosmos::Simulator::set_current(nullptr);
    }

    cosmos::Simulator sim(kSeedA);
    must(sim.install_faults(eagain_config(1.0)).has_value());
    cosmos::Simulator::set_current(&sim);

    unsigned char buf[32];
    for (int i = 0; i < 8; ++i) {
        errno = 0;
        assert(getrandom(buf, sizeof(buf), 0) == -1);
        assert(errno == EAGAIN);
    }
    assert(sim.injector_or_null()->injections(cosmos::SiteId::getrandom) == 8);

    sim.injector_or_null()->push_quiet();
    assert(getrandom(buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
    assert(memcmp(baseline, buf, sizeof(baseline)) == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_eagain_does_not_consume_the_user_stream" << std::endl;
}

void test_rand_shares_user_stream_with_random() {
    // rand() and random() consume the same User stream in call order.
    cosmos::Rng expected(cosmos::stream_seed(kSeedA, cosmos::StreamDomain::User));
    const int want_rand = static_cast<int>(expected.range(0, static_cast<uint64_t>(RAND_MAX)));
    const long want_random = static_cast<long>(expected.range(0, static_cast<uint64_t>(RAND_MAX)));

    int got_rand = 0;
    long got_random = 0;
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        got_rand = rand();
        got_random = random();
        cosmos::Simulator::set_current(nullptr);
    }
    assert(got_rand == want_rand);
    assert(got_random == want_random);

    // And the interleaved sequence replays for the same seed.
    int replay_rand[64];
    int replay_rand2[64];
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        for (int& v : replay_rand) {
            v = rand();
            assert(v >= 0 && v <= RAND_MAX);
        }
        cosmos::Simulator::set_current(nullptr);
    }
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        for (int& v : replay_rand2) {
            v = rand();
        }
        cosmos::Simulator::set_current(nullptr);
    }
    assert(memcmp(replay_rand, replay_rand2, sizeof(replay_rand)) == 0);

    std::cout << "[PASS] test_rand_shares_user_stream_with_random" << std::endl;
}

void test_srandom_srand_are_noops() {
    struct Sample {
        long from_random;
        int from_rand;
    };
    Sample without[16];
    Sample with[16];
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        for (Sample& v : without) {
            v.from_random = random();
            v.from_rand = rand();
        }
        cosmos::Simulator::set_current(nullptr);
    }
    {
        cosmos::Simulator sim(kSeedA);
        cosmos::Simulator::set_current(&sim);
        srandom(999);
        srand(111);
        for (Sample& v : with) {
            v.from_random = random();
            v.from_rand = rand();
        }
        // Seeding calls must not advance the User stream either.
        srandom(42);
        srand(43);
        Sample after{random(), rand()};
        cosmos::Simulator::set_current(nullptr);
        // Member-wise, not memcmp: Sample has padding bytes that neither loop writes.
        for (size_t i = 0; i < 16; ++i) {
            assert(without[i].from_random == with[i].from_random);
            assert(without[i].from_rand == with[i].from_rand);
        }
        (void)after;
    }
    std::cout << "[PASS] test_srandom_srand_are_noops" << std::endl;
}

void test_no_eagain_without_injector() {
    cosmos::Simulator sim(kSeedA);
    assert(!sim.has_injector());
    cosmos::Simulator::set_current(&sim);
    unsigned char buf[16];
    for (int i = 0; i < 1000; ++i) {
        errno = 0;
        assert(getrandom(buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
        assert(errno == 0);
    }
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_no_eagain_without_injector" << std::endl;
}

// Rule 7: engine-internal randomness must not be faulted, and must not spend the site's budget.
void test_getrandom_inside_wrapper_logic_is_never_faulted() {
    cosmos::Simulator sim(kSeedA);
    must(sim.install_faults(eagain_config(1.0)).has_value());
    cosmos::Simulator::set_current(&sim);

    unsigned char buf[16];
    {
        cosmos::wrappers::ReentrancyGuard guard;
        errno = 0;
        assert(getrandom(buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
        assert(errno == 0);
    }
    assert(sim.injector_or_null()->eligible_calls(cosmos::SiteId::getrandom) == 0);

    errno = 0;
    assert(getrandom(buf, sizeof(buf), 0) == -1);
    assert(errno == EAGAIN);
    assert(sim.injector_or_null()->eligible_calls(cosmos::SiteId::getrandom) == 1);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_getrandom_inside_wrapper_logic_is_never_faulted" << std::endl;
}

void test_wrapper_does_not_allocate() {
    cosmos::Simulator sim(kSeedA);
    cosmos::Simulator::set_current(&sim);
    unsigned char buf[16];
    for (int i = 0; i < 10000; ++i) {
        assert(getrandom(buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
        long v = random();
        assert(v >= 0 && v <= RAND_MAX);
        int r = rand();
        assert(r >= 0 && r <= RAND_MAX);
    }
    // No malloc/free in the wrapper path: sim heap untouched.
    assert(sim.heap().stats().active_allocations == 0);
    assert(sim.heap().stats().total_allocation_count == 0);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_wrapper_does_not_allocate" << std::endl;
}

int main() {
    test_passthrough_no_sim();
    test_getrandom_arg_errors();
    test_getrandom_flags_accepted();
    test_getrandom_determinism_same_seed();
    test_getrandom_matches_user_stream();
    test_getrandom_different_seed_differs();
    test_random_range_and_replay();
    test_rand_shares_user_stream_with_random();
    test_user_stream_isolated_from_fault_stream();
    test_eagain_does_not_consume_the_user_stream();
    test_srandom_srand_are_noops();
    test_no_eagain_without_injector();
    test_getrandom_inside_wrapper_logic_is_never_faulted();
    test_wrapper_does_not_allocate();
    std::cout << "All random wrapper tests passed successfully!" << std::endl;
    return 0;
}
