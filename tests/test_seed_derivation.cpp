#include "cosmos/faults.hpp"
#include "cosmos/random.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

constexpr uint64_t SAMPLE_COUNT = 200000;

struct PairStats {
    double correlation;
    double mean_hamming_distance;
};

PairStats compare_streams(uint64_t seed_a, uint64_t seed_b) {
    cosmos::Rng a(seed_a);
    cosmos::Rng b(seed_b);

    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_yy = 0, sum_xy = 0, hamming_total = 0;
    for (uint64_t i = 0; i < SAMPLE_COUNT; ++i) {
        uint64_t raw_a = a.next();
        uint64_t raw_b = b.next();
        hamming_total += std::popcount(raw_a ^ raw_b);

        double x = static_cast<double>(raw_a >> 11) * 0x1.0p-53;
        double y = static_cast<double>(raw_b >> 11) * 0x1.0p-53;
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_yy += y * y;
        sum_xy += x * y;
    }

    double n = static_cast<double>(SAMPLE_COUNT);
    double correlation = (n * sum_xy - sum_x * sum_y) /
                         std::sqrt((n * sum_xx - sum_x * sum_x) * (n * sum_yy - sum_y * sum_y));
    return {correlation, hamming_total / n};
}

// Independent uniform streams: correlation ~ N(0, 1/sqrt(n)), Hamming distance ~ N(32, 4/sqrt(n)).
void assert_uncorrelated(uint64_t seed_a, uint64_t seed_b) {
    assert(seed_a != seed_b);
    PairStats stats = compare_streams(seed_a, seed_b);

    double correlation_limit = 5.0 / std::sqrt(static_cast<double>(SAMPLE_COUNT));
    double hamming_limit = 5.0 * 4.0 / std::sqrt(static_cast<double>(SAMPLE_COUNT));

    assert(std::fabs(stats.correlation) < correlation_limit);
    assert(std::fabs(stats.mean_hamming_distance - 32.0) < hamming_limit);
}

void assert_all_distinct(std::vector<uint64_t> values) {
    std::sort(values.begin(), values.end());
    assert(std::adjacent_find(values.begin(), values.end()) == values.end());
}

std::vector<uint64_t> memory_draws_alongside_network(uint64_t network_draws_per_round) {
    uint64_t fault_seed =
        cosmos::stream_seed(cosmos::universe_seed(4242, 7), cosmos::StreamDomain::Fault);
    cosmos::Rng memory(cosmos::fault_class_seed(fault_seed, cosmos::FaultClass::Memory));
    cosmos::Rng network(cosmos::fault_class_seed(fault_seed, cosmos::FaultClass::Network));

    std::vector<uint64_t> memory_values;
    for (int round = 0; round < 50; ++round) {
        memory_values.push_back(memory.next());
        for (uint64_t i = 0; i < network_draws_per_round; ++i) {
            network.next();
        }
    }
    return memory_values;
}

} // namespace

void test_derivation_is_deterministic() {
    for (int repeat = 0; repeat < 100; ++repeat) {
        assert(cosmos::universe_seed(9, 3) == cosmos::universe_seed(9, 3));
        assert(cosmos::derive_seed(123, 456) == cosmos::derive_seed(123, 456));
        assert(cosmos::stream_seed(77, cosmos::StreamDomain::Fault) ==
               cosmos::stream_seed(77, cosmos::StreamDomain::Fault));
    }
    std::cout << "[PASS] test_derivation_is_deterministic" << std::endl;
}

void test_universe_seeds_are_distinct() {
    std::vector<uint64_t> seeds;
    for (uint64_t campaign = 0; campaign < 200; ++campaign) {
        for (uint64_t index = 0; index < 200; ++index) {
            seeds.push_back(cosmos::universe_seed(campaign, index));
        }
    }
    assert_all_distinct(seeds);

    std::vector<uint64_t> many_indices;
    for (uint64_t index = 0; index < 100000; ++index) {
        many_indices.push_back(cosmos::universe_seed(12345, index));
    }
    assert_all_distinct(many_indices);
    std::cout << "[PASS] test_universe_seeds_are_distinct" << std::endl;
}

void test_adjacent_universe_indices_decorrelate() {
    for (uint64_t index : {uint64_t(0), uint64_t(41), uint64_t(1000)}) {
        assert_uncorrelated(cosmos::universe_seed(7, index), cosmos::universe_seed(7, index + 1));
    }
    assert_uncorrelated(cosmos::universe_seed(100, 5), cosmos::universe_seed(101, 5));
    std::cout << "[PASS] test_adjacent_universe_indices_decorrelate" << std::endl;
}

void test_stream_domains_are_independent() {
    uint64_t universe = cosmos::universe_seed(12345, 99);
    cosmos::StreamDomain domains[] = {cosmos::StreamDomain::Schedule, cosmos::StreamDomain::Fault,
                                      cosmos::StreamDomain::Workload, cosmos::StreamDomain::User,
                                      cosmos::StreamDomain::Swarm};

    std::vector<uint64_t> seeds;
    for (cosmos::StreamDomain domain : domains) {
        seeds.push_back(cosmos::stream_seed(universe, domain));
    }
    assert_all_distinct(seeds);

    assert_uncorrelated(cosmos::stream_seed(universe, cosmos::StreamDomain::Schedule),
                        cosmos::stream_seed(universe, cosmos::StreamDomain::Fault));
    assert_uncorrelated(cosmos::stream_seed(universe, cosmos::StreamDomain::Fault),
                        cosmos::stream_seed(universe, cosmos::StreamDomain::Swarm));
    std::cout << "[PASS] test_stream_domains_are_independent" << std::endl;
}

void test_fault_class_substreams_are_isolated() {
    std::vector<uint64_t> baseline = memory_draws_alongside_network(0);
    assert(baseline.size() == 50);

    // The Rule 2 guarantee: however much the Network sub-stream is consumed, the k-th Memory
    // draw returns the same value.
    for (uint64_t network_draws : {uint64_t(1), uint64_t(17), uint64_t(1000)}) {
        assert(memory_draws_alongside_network(network_draws) == baseline);
    }
    std::cout << "[PASS] test_fault_class_substreams_are_isolated" << std::endl;
}

void test_every_fault_class_derives_a_distinct_substream() {
    uint64_t fault_seed =
        cosmos::stream_seed(cosmos::universe_seed(1, 1), cosmos::StreamDomain::Fault);

    std::vector<uint64_t> seeds;
    for (uint8_t raw = 0; raw < static_cast<uint8_t>(cosmos::FaultClass::_Count); ++raw) {
        seeds.push_back(cosmos::fault_class_seed(fault_seed, static_cast<cosmos::FaultClass>(raw)));
    }
    assert(seeds.size() == static_cast<size_t>(cosmos::FaultClass::_Count));
    assert_all_distinct(seeds);

    // The last real class sits at _Count - 1; deriving for it must not fall off the end.
    uint64_t last = cosmos::fault_class_seed(
        fault_seed,
        static_cast<cosmos::FaultClass>(static_cast<uint8_t>(cosmos::FaultClass::_Count) - 1));
    assert(last == cosmos::fault_class_seed(fault_seed, cosmos::FaultClass::Random));

    assert_uncorrelated(cosmos::fault_class_seed(fault_seed, cosmos::FaultClass::Memory),
                        cosmos::fault_class_seed(fault_seed, cosmos::FaultClass::Network));
    assert_uncorrelated(cosmos::fault_class_seed(fault_seed, cosmos::FaultClass::Clock),
                        cosmos::fault_class_seed(fault_seed, cosmos::FaultClass::Process));
    std::cout << "[PASS] test_every_fault_class_derives_a_distinct_substream" << std::endl;
}

void test_zero_campaign_seed_and_index_are_not_degenerate() {
    uint64_t universe = cosmos::universe_seed(0, 0);
    assert(universe != 0);

    cosmos::Rng rng(universe);
    bool saw_nonzero = false;
    for (int i = 0; i < 100; ++i) {
        if (rng.next() != 0) {
            saw_nonzero = true;
            break;
        }
    }
    assert(saw_nonzero);

    assert(cosmos::universe_seed(0, 0) != cosmos::universe_seed(0, 1));
    assert(cosmos::universe_seed(0, 0) != cosmos::universe_seed(1, 0));
    assert(cosmos::stream_seed(0, cosmos::StreamDomain::Schedule) != 0);
    assert(cosmos::derive_seed(0, 0) != 0);
    std::cout << "[PASS] test_zero_campaign_seed_and_index_are_not_degenerate" << std::endl;
}

void test_extreme_index_values() {
    assert(cosmos::universe_seed(UINT64_MAX, UINT64_MAX) != 0);
    assert(cosmos::universe_seed(UINT64_MAX, UINT64_MAX) != cosmos::universe_seed(UINT64_MAX, 0));
    assert_uncorrelated(cosmos::universe_seed(0, UINT64_MAX),
                        cosmos::universe_seed(0, UINT64_MAX - 1));
    std::cout << "[PASS] test_extreme_index_values" << std::endl;
}

int main() {
    test_derivation_is_deterministic();
    test_universe_seeds_are_distinct();
    test_adjacent_universe_indices_decorrelate();
    test_stream_domains_are_independent();
    test_fault_class_substreams_are_isolated();
    test_every_fault_class_derives_a_distinct_substream();
    test_zero_campaign_seed_and_index_are_not_degenerate();
    test_extreme_index_values();
    std::cout << "All seed derivation tests passed successfully!" << std::endl;
    return 0;
}
