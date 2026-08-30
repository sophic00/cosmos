#include "cosmos/random.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>

// Cross-checked against an independent Python re-implementation of the published
// splitmix64 / xoshiro256** algorithms (Blackman & Vigna), not hand-recalled constants.
void test_known_answer_vectors() {
    struct Case {
        uint64_t seed;
        uint64_t expected[5];
    };

    Case cases[] = {
        {0x0ULL,
         {0x99ec5f36cb75f2b4ULL, 0xbf6e1f784956452aULL, 0x1a5f849d4933e6e0ULL,
          0x6aa594f1262d2d2cULL, 0xbba5ad4a1f842e59ULL}},
        {0x1ULL,
         {0xb3f2af6d0fc710c5ULL, 0x853b559647364ceaULL, 0x92f89756082a4514ULL,
          0x642e1c7bc266a3a7ULL, 0xb27a48e29a233673ULL}},
        {0x2aULL,
         {0x15780b2e0c2ec716ULL, 0x6104d9866d113a7eULL, 0xae17533239e499a1ULL,
          0xecb8ad4703b360a1ULL, 0xfde6dc7fe2ec5e64ULL}},
        {0xffffffffffffffffULL,
         {0x8f5520d52a7ead08ULL, 0xc476a018caa1802dULL, 0x81de31c0d260469eULL,
          0xbf658d7e065f3c2fULL, 0x913593fda1bca32aULL}},
    };

    for (const auto& c : cases) {
        cosmos::Rng rng(c.seed);
        for (uint64_t expected : c.expected) {
            assert(rng.next() == expected);
        }
    }
    std::cout << "[PASS] test_known_answer_vectors" << std::endl;
}

void test_same_seed_reproduces_identical_sequence() {
    cosmos::Rng a(123456789ULL);
    cosmos::Rng b(123456789ULL);

    for (int i = 0; i < 10000; ++i) {
        assert(a.next() == b.next());
    }
    std::cout << "[PASS] test_same_seed_reproduces_identical_sequence" << std::endl;
}

void test_uniform_stays_within_unit_interval() {
    cosmos::Rng rng(987654321ULL);

    for (int i = 0; i < 1000000; ++i) {
        double value = rng.uniform();
        assert(value >= 0.0);
        assert(value < 1.0);
    }
    std::cout << "[PASS] test_uniform_stays_within_unit_interval" << std::endl;
}

void test_seed_zero_is_not_degenerate() {
    cosmos::Rng rng(0ULL);

    bool saw_nonzero = false;
    for (int i = 0; i < 100; ++i) {
        if (rng.next() != 0) {
            saw_nonzero = true;
            break;
        }
    }
    assert(saw_nonzero);
    std::cout << "[PASS] test_seed_zero_is_not_degenerate" << std::endl;
}

void test_range_respects_bounds() {
    cosmos::Rng rng(42ULL);

    for (int i = 0; i < 100000; ++i) {
        uint64_t value = rng.range(10, 20);
        assert(value >= 10 && value <= 20);
    }

    // Degenerate single-value range never drifts outside it.
    for (int i = 0; i < 1000; ++i) {
        assert(rng.range(7, 7) == 7);
    }

    // Full 64-bit span (lo=0, hi=UINT64_MAX) must not loop forever or crash.
    for (int i = 0; i < 1000; ++i) {
        rng.range(0, UINT64_MAX);
    }
    std::cout << "[PASS] test_range_respects_bounds" << std::endl;
}

void test_coin_respects_extremes() {
    cosmos::Rng rng(1ULL);

    for (int i = 0; i < 10000; ++i) {
        assert(rng.coin(0.0) == false);
    }
    for (int i = 0; i < 10000; ++i) {
        assert(rng.coin(1.0) == true);
    }
    std::cout << "[PASS] test_coin_respects_extremes" << std::endl;
}

int main() {
    test_known_answer_vectors();
    test_same_seed_reproduces_identical_sequence();
    test_uniform_stays_within_unit_interval();
    test_seed_zero_is_not_degenerate();
    test_range_respects_bounds();
    test_coin_respects_extremes();
    std::cout << "All random engine tests passed successfully!" << std::endl;
    return 0;
}
