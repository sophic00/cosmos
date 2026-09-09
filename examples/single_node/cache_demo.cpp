/*
 * Cache demo: one source, two binaries.
 *
 * Build selects the mode (same pattern as kv_store_prod/sim):
 *   cache_demo_prod: -DCOSMOS_PROD, no libcosmos link. Plain direct calls to
 *     the fixed cache app; every malloc succeeds in practice, so all 20 keys
 *     are stored and found. The "happy path" that never exercises OOM.
 *   cache_demo_sim: -DCOSMOS_SIM, links libcosmos with -Wl,--wrap on the
 *     allocator. The same workload runs inside a Scenario universe where one
 *     allocation fails deterministically. --seed decides the fault beforehand:
 *     seed % 22 == 0 means clean, 1 means cache_create fails, 2..21 means that
 *     PUT fails. Rate is 0, so no RNG draws are consumed.
 */
#include <cstdio>
#include <cstring>

#include "broken_cache.h"

constexpr int kDemoKeys = 20;

#ifdef COSMOS_SIM

#include "cosmos/scenario.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

// 1x cache_create + 20x put. Nothing else in the workload allocates, which is
// what makes fail_on_call exact (see tests/test_broken_app.cpp).
constexpr uint64_t kEligibleCalls = 1 + kDemoKeys;
// One slot per eligible call plus slot 0 = no fault.
constexpr uint64_t kSeedSlots = 1 + kEligibleCalls;

// Beforehand calculation: which eligible malloc (if any) this seed will fail.
// Nullopt arms nothing; otherwise the K-th eligible malloc fails once with
// OutOfMemory. Pure arithmetic, no RNG involved.
std::optional<uint64_t> seed_to_fail_on(uint64_t seed) {
    uint64_t slot = seed % kSeedSlots;
    if (slot == 0) return std::nullopt;
    return slot;
}

std::string describe_plan(uint64_t seed, std::optional<uint64_t> fail_on) {
    std::ostringstream out;
    out << seed << " % " << kSeedSlots << " = " << (seed % kSeedSlots);
    if (!fail_on.has_value()) {
        out << " -> no fault injected (clean run)";
        return out.str();
    }
    out << " -> fail malloc #" << *fail_on << " once with OutOfMemory";
    if (*fail_on == 1) {
        out << " (cache_create)";
    } else {
        int key = static_cast<int>(*fail_on - 2);
        char name[CACHE_KEY_MAX];
        std::snprintf(name, CACHE_KEY_MAX, "key%02d", key);
        out << " (PUT " << name << ")";
    }
    return out.str();
}

cosmos::FaultPlan oom_at(std::optional<uint64_t> fail_on_call) {
    cosmos::FaultPlan plan;
    if (!fail_on_call.has_value()) return plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    (void)plan.activate_site(cosmos::SiteId::malloc);
    cosmos::FaultRule rule;
    (void)rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0);
    rule.fire_on_eligible_call = fail_on_call;
    (void)plan.set_rule(cosmos::SiteId::malloc, std::move(rule));
    return plan;
}

// Built before the universe starts: formatting inside the workload would add
// allocations the eligible count is not expecting.
struct Keys {
    char key[kDemoKeys][CACHE_KEY_MAX];
    char value[kDemoKeys][CACHE_VALUE_MAX];

    Keys() {
        for (int i = 0; i < kDemoKeys; ++i) {
            std::snprintf(key[i], CACHE_KEY_MAX, "key%02d", i);
            std::snprintf(value[i], CACHE_VALUE_MAX, "value%02d", i);
        }
    }
};

struct Fixture {
    Cache* cache = nullptr;
    int put_rc[kDemoKeys] = {};
    bool put_ok[kDemoKeys] = {};
    bool got_ok[kDemoKeys] = {};

    void release() {
        cache_destroy(cache);
        cache = nullptr;
    }
};

bool every_acknowledged_key_survived(const Keys& keys, Fixture& fixture) {
    if (fixture.cache == nullptr) return false;
    bool all = true;
    for (int i = 0; i < kDemoKeys; ++i) {
        if (!fixture.put_ok[i]) {
            fixture.got_ok[i] = true; // not acknowledged, nothing to verify
            continue;
        }
        const char* got = cache_get(fixture.cache, keys.key[i]);
        bool found = got != nullptr && std::strcmp(got, keys.value[i]) == 0;
        fixture.got_ok[i] = found;
        if (!found) all = false;
    }
    return all;
}

void print_help(const char* argv0) {
    std::cout << "Usage: " << argv0 << " --seed N\n\n"
              << "Seed decides the fault before the run starts (seed % 22):\n"
              << "  0         -> no fault (clean run)\n"
              << "  1         -> cache_create runs out of memory (cannot be tolerated)\n"
              << "  2..21     -> that PUT runs out of memory (fixed app tolerates it)\n\n"
              << "Try:\n"
              << "  " << argv0 << " --seed 1006   # 1006 % 22 = 16, PUT key14 fails, tolerated\n"
              << "  " << argv0
              << " --seed 991    # 991 % 22 = 1, cache_create fails, not handled\n";
}

} // namespace

int main(int argc, char** argv) {
    uint64_t seed = 1006;
    bool seed_given = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        }
        if ((arg == "--seed" || arg == "-s") && i + 1 < argc) {
            seed = static_cast<uint64_t>(std::stoull(argv[++i]));
            seed_given = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_help(argv[0]);
            return 2;
        }
    }
    if (!seed_given) {
        std::cout << "(no --seed given, defaulting to 1006)\n";
    }

    static const Keys keys;
    Fixture fixture;
    std::optional<uint64_t> fail_on = seed_to_fail_on(seed);

    std::cout << "Cosmos cache demo -- simulated run (libcosmos, fixed app)\n";
    std::cout << "Seed " << describe_plan(seed, fail_on) << "\n";
    std::cout << "Rate is 0, so exactly "
              << (fail_on.has_value() ? "1 fault fires" : "0 faults fire")
              << " and no RNG draws are consumed.\n\n";

    auto scenario = cosmos::Scenario::create(seed, oom_at(fail_on));
    if (!scenario) {
        std::cerr << "Invalid fault plan\n";
        return 2;
    }

    // Workload: no printing or formatting in here, only the app's own mallocs.
    scenario->run([&] {
        fixture.cache = cache_create();
        if (fixture.cache == nullptr) return;
        for (int i = 0; i < kDemoKeys; ++i) {
            fixture.put_rc[i] = cache_put(fixture.cache, keys.key[i], keys.value[i]);
            fixture.put_ok[i] = fixture.put_rc[i] == CACHE_OK;
        }
    });

    scenario->quiesce();
    scenario->check("no-acknowledged-key-lost",
                    [&] { return every_acknowledged_key_survived(keys, fixture); });

    if (fixture.cache == nullptr) {
        std::cout << "CREATE FAILED: out of memory (cache_create returned NULL)\n";
    } else {
        for (int i = 0; i < kDemoKeys; ++i) {
            const char* status = "UNKNOWN";
            if (fixture.put_rc[i] == CACHE_OK)
                status = "[OK]";
            else if (fixture.put_rc[i] == CACHE_NO_MEMORY)
                status = "[OUT OF MEMORY - told the truth]";
            else if (fixture.put_rc[i] == CACHE_FULL)
                status = "[FULL]";
            std::cout << "PUT " << keys.key[i] << " -> " << status << "\n";
        }
    }

    std::cout << "\nVerifying every key the app said it stored...\n";
    int missing = 0;
    if (fixture.cache == nullptr) {
        std::cout << "Nothing to verify: the cache was never created.\n";
    } else {
        for (int i = 0; i < kDemoKeys; ++i) {
            if (!fixture.put_ok[i]) {
                std::cout << "GET " << keys.key[i] << " -> [skipped, app reported no-memory]\n";
                continue;
            }
            std::cout << "GET " << keys.key[i] << " -> "
                      << (fixture.got_ok[i] ? "[found]" : "[MISSING - app lied]") << "\n";
            if (!fixture.got_ok[i]) ++missing;
        }
    }

    const auto& report = scenario->report();
    uint64_t eligible = report.eligible_calls[cosmos::site_slot(cosmos::SiteId::malloc)];
    uint64_t injections = report.injections[cosmos::site_slot(cosmos::SiteId::malloc)];

    std::cout << "\nEligible mallocs: " << eligible << ", faults injected: " << injections << "\n";
    if (scenario->passed()) {
        std::cout << "Result: TOLERATED -- every acknowledged key survived."
                  << " Run again with --seed " << seed << " to replay this exact universe.\n";
    } else {
        std::cout << "Result: NOT HANDLED -- the fault escaped handling."
                  << " Run again with --seed " << seed << " to replay this exact universe.\n";
        std::cout << "\nEngineer detail:\n";
        cosmos::print_report(std::cout, *scenario);
    }

    fixture.release();
    return scenario->passed() ? 0 : 1;
}

#else

int main() {
    char keys[kDemoKeys][CACHE_KEY_MAX];
    char values[kDemoKeys][CACHE_VALUE_MAX];
    for (int i = 0; i < kDemoKeys; ++i) {
        snprintf(keys[i], CACHE_KEY_MAX, "key%02d", i);
        snprintf(values[i], CACHE_VALUE_MAX, "value%02d", i);
    }

    printf("Cosmos cache demo -- normal run (no libcosmos)\n");
    printf("Putting %d keys into the cache...\n\n", kDemoKeys);

    Cache* cache = cache_create();
    if (cache == NULL) {
        printf("CREATE FAILED: out of memory\n");
        return 1;
    }

    int put_ok[kDemoKeys];
    for (int i = 0; i < kDemoKeys; ++i) {
        int rc = cache_put(cache, keys[i], values[i]);
        put_ok[i] = (rc == CACHE_OK);
        printf("PUT %-6s -> %s\n", keys[i], put_ok[i] ? "[OK]" : "[FAILED]");
    }

    printf("\nVerifying every key the app said it stored...\n");
    int missing = 0;
    for (int i = 0; i < kDemoKeys; ++i) {
        if (!put_ok[i]) continue;
        const char* got = cache_get(cache, keys[i]);
        int found = got != NULL && strcmp(got, values[i]) == 0;
        if (!found) ++missing;
        printf("GET %-6s -> %s\n", keys[i], found ? "[found]" : "[MISSING]");
    }

    printf("\nResult: %d keys stored, %d missing. No faults were possible in this run.\n",
           kDemoKeys, missing);
    cache_destroy(cache);
    return missing == 0 ? 0 : 1;
}

#endif
