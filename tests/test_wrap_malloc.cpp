#include "cosmos/cosmos.hpp"
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

void test_passthrough_malloc() {
    assert(!cosmos::Simulator::has_current());
    void* ptr = malloc(64);
    assert(ptr != nullptr);
    free(ptr);
    std::cout << "[PASS] test_passthrough_malloc" << std::endl;
}

void test_active_sim_malloc_and_alignment() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    std::size_t test_sizes[] = {0, 1, 3, 7, 15, 16, 17, 31, 32, 33, 64, 1024};
    std::vector<void*> ptrs;

    for (std::size_t size : test_sizes) {
        void* ptr = malloc(size);
        assert(ptr != nullptr);

        // Strict 16-byte alignment assertion (alignof(std::max_align_t))
        std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ptr);
        assert(addr % alignof(std::max_align_t) == 0);

        // Verify canary magic in header preceding payload
        auto* header = reinterpret_cast<cosmos::AllocationHeader*>(
            static_cast<char*>(ptr) - sizeof(cosmos::AllocationHeader));
        assert(header->magic == cosmos::COSMOS_CANARY_MAGIC);
        assert(header->requested_size == size);

        ptrs.push_back(ptr);
    }

    assert(sim.heap().stats().active_allocations == ptrs.size());
    assert(sim.heap().stats().total_allocation_count == ptrs.size());

    for (void* p : ptrs) {
        free(p);
    }

    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_active_sim_malloc_and_alignment" << std::endl;
}

void test_oom_fault_injection() {
    cosmos::Simulator sim;
    cosmos::FaultProfile fp;
    fp.oom_rate = 1.0; // 100% OOM injection
    sim.set_faults(fp);
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    void* ptr = malloc(128);
    assert(ptr == nullptr);
    assert(errno == ENOMEM);
    assert(sim.heap().stats().oom_fault_count == 1);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_oom_fault_injection" << std::endl;
}

void test_zero_byte_allocation() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void* ptr = malloc(0);
    assert(ptr != nullptr);
    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ptr);
    assert(addr % alignof(std::max_align_t) == 0);

    free(ptr);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_zero_byte_allocation" << std::endl;
}

void test_reentrancy_and_many_allocations() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    // Stress test container insertion inside TrackedHeap with 10,000 allocations
    std::vector<void*> ptrs;
    ptrs.reserve(10000);

    for (int i = 0; i < 10000; ++i) {
        void* p = malloc(i % 128 + 1);
        assert(p != nullptr);
        ptrs.push_back(p);
    }

    assert(sim.heap().stats().active_allocations == 10000);

    for (void* p : ptrs) {
        free(p);
    }

    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_reentrancy_and_many_allocations" << std::endl;
}

void test_calloc_zero_init_and_tracking() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    struct Case {
        std::size_t nmemb;
        std::size_t size;
    };
    Case cases[] = {{1, 1}, {7, 3}, {16, 4}, {0, 8}, {8, 0}, {0, 0}, {64, 64}};

    std::vector<void*> ptrs;
    for (const Case& c : cases) {
        void* ptr = calloc(c.nmemb, c.size);
        assert(ptr != nullptr);
        assert(reinterpret_cast<std::uintptr_t>(ptr) % alignof(std::max_align_t) == 0);

        std::size_t total = c.nmemb * c.size;
        const unsigned char* bytes = static_cast<const unsigned char*>(ptr);
        for (std::size_t i = 0; i < total; ++i) {
            assert(bytes[i] == 0);
        }

        auto* header = reinterpret_cast<cosmos::AllocationHeader*>(
            static_cast<char*>(ptr) - sizeof(cosmos::AllocationHeader));
        assert(header->magic == cosmos::COSMOS_CANARY_MAGIC);
        assert(header->requested_size == total);

        ptrs.push_back(ptr);
    }

    assert(sim.heap().stats().active_allocations == ptrs.size());
    assert(sim.heap().stats().total_allocation_count == ptrs.size());
    assert(sim.heap().stats().oom_fault_count == 0);

    for (void* p : ptrs) {
        free(p);
    }

    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_calloc_zero_init_and_tracking" << std::endl;
}

void test_calloc_overflow_rejected() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    void* ptr = calloc(SIZE_MAX / 2, 3);
    assert(ptr == nullptr);
    assert(errno == ENOMEM);
    // Overflow is a legitimate API failure, not an injected fault.
    assert(sim.heap().stats().oom_fault_count == 0);
    assert(sim.heap().stats().total_allocation_count == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_calloc_overflow_rejected" << std::endl;
}

void test_calloc_oom_fault_injection() {
    cosmos::Simulator sim;
    cosmos::FaultProfile fp;
    fp.oom_rate = 1.0; // 100% OOM injection
    sim.set_faults(fp);
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    void* ptr = calloc(4, 32);
    assert(ptr == nullptr);
    assert(errno == ENOMEM);
    assert(sim.heap().stats().oom_fault_count == 1);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_calloc_oom_fault_injection" << std::endl;
}

void test_realloc_null_behaves_as_malloc() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void* ptr = realloc(nullptr, 100);
    assert(ptr != nullptr);
    assert(reinterpret_cast<std::uintptr_t>(ptr) % alignof(std::max_align_t) == 0);

    auto* header = reinterpret_cast<cosmos::AllocationHeader*>(static_cast<char*>(ptr) -
                                                               sizeof(cosmos::AllocationHeader));
    assert(header->magic == cosmos::COSMOS_CANARY_MAGIC);
    assert(header->requested_size == 100);

    assert(sim.heap().stats().active_allocations == 1);
    assert(sim.heap().stats().total_allocation_count == 1);

    free(ptr);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_realloc_null_behaves_as_malloc" << std::endl;
}

void test_realloc_grow_and_shrink_preserve_data() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    auto* p = static_cast<unsigned char*>(realloc(nullptr, 32));
    assert(p != nullptr);
    for (int i = 0; i < 32; ++i) {
        p[i] = static_cast<unsigned char>(i);
    }

    auto* q = static_cast<unsigned char*>(realloc(p, 128));
    assert(q != nullptr);
    for (int i = 0; i < 32; ++i) {
        assert(q[i] == static_cast<unsigned char>(i));
    }
    assert(!sim.heap().owns(p)); // old block released
    auto* q_header = reinterpret_cast<cosmos::AllocationHeader*>(reinterpret_cast<std::byte*>(q) -
                                                                 sizeof(cosmos::AllocationHeader));
    assert(q_header->requested_size == 128);

    auto* r = static_cast<unsigned char*>(realloc(q, 8));
    assert(r != nullptr);
    for (int i = 0; i < 8; ++i) {
        assert(r[i] == static_cast<unsigned char>(i));
    }
    assert(!sim.heap().owns(q));
    auto* r_header = reinterpret_cast<cosmos::AllocationHeader*>(reinterpret_cast<std::byte*>(r) -
                                                                 sizeof(cosmos::AllocationHeader));
    assert(r_header->requested_size == 8);

    // Each realloc allocates one fresh tracked block and frees the old one.
    assert(sim.heap().stats().active_allocations == 1);
    assert(sim.heap().stats().total_allocation_count == 3);

    free(r);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_realloc_grow_and_shrink_preserve_data" << std::endl;
}

void test_realloc_zero_size_frees() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void* ptr = malloc(64);
    assert(ptr != nullptr);
    assert(sim.heap().owns(ptr));

    void* result = realloc(ptr, 0);
    assert(result == nullptr); // glibc-compatible: freed, no pending exception errno
    assert(sim.heap().stats().active_allocations == 0);
    assert(!sim.heap().owns(ptr));

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_realloc_zero_size_frees" << std::endl;
}

void test_realloc_oom_leaves_original_intact() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    auto* p = static_cast<unsigned char*>(malloc(32));
    assert(p != nullptr);
    for (int i = 0; i < 32; ++i) {
        p[i] = static_cast<unsigned char>(i ^ 0x5A);
    }

    cosmos::FaultProfile fp;
    fp.oom_rate = 1.0; // arm OOM only for the resize
    sim.set_faults(fp);

    errno = 0;
    void* q = realloc(p, 4096);
    assert(q == nullptr);
    assert(errno == ENOMEM);
    assert(sim.heap().stats().oom_fault_count == 1);
    assert(sim.heap().owns(p)); // original block still valid and tracked
    for (int i = 0; i < 32; ++i) {
        assert(p[i] == static_cast<unsigned char>(i ^ 0x5A));
    }
    assert(sim.heap().stats().active_allocations == 1);

    free(p);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_realloc_oom_leaves_original_intact" << std::endl;
}

void test_realloc_passthrough_pointer() {
    void* p = malloc(64); // host heap, allocated outside any simulation
    assert(p != nullptr);
    memset(p, 0xAB, 64);

    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void* q = realloc(p, 256); // untracked inside sim -> __real_realloc
    assert(q != nullptr);
    const unsigned char* bytes = static_cast<const unsigned char*>(q);
    for (int i = 0; i < 64; ++i) {
        assert(bytes[i] == 0xAB);
    }
    assert(sim.heap().stats().total_allocation_count == 0);
    assert(sim.heap().stats().active_allocations == 0);

    void* r = realloc(q, 0); // untracked zero-size realloc -> __real_free
    assert(r == nullptr);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_realloc_passthrough_pointer" << std::endl;
}

void test_free_passthrough_pointer_inside_sim() {
    void* ptr = malloc(16); // host heap
    assert(ptr != nullptr);

    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    free(ptr); // must not read a header; falls back to __real_free
    assert(sim.heap().stats().total_allocation_count == 0);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_free_passthrough_pointer_inside_sim" << std::endl;
}

// A pointer allocated under sim A but freed while no simulator is current must still update A's
// stats. Ownership routing via the registry, not the "currently active" heap, decides.
void test_free_after_sim_deactivation_routes_to_owner() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void* ptr = malloc(128);
    assert(ptr != nullptr);
    assert(sim.heap().stats().active_allocations == 1);

    cosmos::Simulator::set_current(nullptr);
    assert(!cosmos::Simulator::has_current());

    free(ptr);
    assert(sim.heap().stats().active_allocations == 0);

    std::cout << "[PASS] test_free_after_sim_deactivation_routes_to_owner" << std::endl;
}

// Freeing A's pointer while B is current must update A's stats and leave B untouched; the old
// behavior routed to B's heap, missed, and orphaned the accounting.
void test_free_under_foreign_sim_updates_original_owner() {
    cosmos::Simulator sim_a;
    cosmos::Simulator::set_current(&sim_a);
    void* a_ptr = malloc(64);
    assert(sim_a.heap().stats().active_allocations == 1);

    cosmos::Simulator sim_b;
    cosmos::Simulator::set_current(&sim_b);
    void* b_ptr = malloc(32);
    assert(sim_b.heap().stats().active_allocations == 1);

    free(a_ptr);
    assert(sim_a.heap().stats().active_allocations == 0);
    assert(sim_b.heap().stats().active_allocations == 1);

    free(b_ptr);
    assert(sim_b.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_free_under_foreign_sim_updates_original_owner" << std::endl;
}

// realloc with no simulator current must resize through the owning heap, not corrupt by handing
// a header-offset payload to __real_realloc.
void test_realloc_after_sim_deactivation_routes_to_owner() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    auto* ptr = static_cast<char*>(malloc(8));
    assert(ptr != nullptr);
    strcpy(ptr, "cosmos");

    cosmos::Simulator::set_current(nullptr);

    auto* grown = static_cast<char*>(realloc(ptr, 64));
    assert(grown != nullptr);
    assert(strcmp(grown, "cosmos") == 0);
    assert(sim.heap().stats().active_allocations == 1); // old freed, fresh block tracked
    assert(sim.heap().stats().total_allocation_count == 2);

    free(grown);
    assert(sim.heap().stats().active_allocations == 0);

    std::cout << "[PASS] test_realloc_after_sim_deactivation_routes_to_owner" << std::endl;
}

// Same routing under a foreign active simulator: the owning heap performs the resize and keeps
// the accounting; the foreign heap is untouched.
void test_realloc_under_foreign_sim_routes_to_owner() {
    cosmos::Simulator sim_a;
    cosmos::Simulator::set_current(&sim_a);
    void* ptr = malloc(16);
    assert(ptr != nullptr);

    cosmos::Simulator sim_b;
    cosmos::Simulator::set_current(&sim_b);

    void* grown = realloc(ptr, 128);
    assert(grown != nullptr);
    assert(sim_a.heap().stats().active_allocations == 1);
    assert(sim_a.heap().stats().total_allocation_count == 2);
    assert(sim_b.heap().stats().active_allocations == 0);
    assert(sim_b.heap().stats().total_allocation_count == 0);

    free(grown);
    assert(sim_a.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_realloc_under_foreign_sim_routes_to_owner" << std::endl;
}

// A block still allocated when its heap is destroyed is orphaned: a later free must release it
// via its header instead of dereferencing the dead heap.
void test_free_after_sim_destruction_orphans_block() {
    void* tracked = nullptr;
    {
        cosmos::Simulator sim;
        cosmos::Simulator::set_current(&sim);
        tracked = malloc(48);
        assert(tracked != nullptr);
        assert(sim.heap().stats().active_allocations == 1);
        // Deliberately no cleanup: stats freeze at 1, the block is orphaned on destruction.
    }

    free(tracked);

    // The host allocator must be intact afterwards.
    void* after = malloc(64);
    assert(after != nullptr);
    free(after);

    std::cout << "[PASS] test_free_after_sim_destruction_orphans_block" << std::endl;
}

// realloc of an orphaned block is emulated (fresh allocation + copy + header-verified release).
// With a fresh simulator active, the new block joins that sim's heap.
void test_realloc_after_sim_destruction_with_new_sim() {
    void* tracked = nullptr;
    {
        cosmos::Simulator old_sim;
        cosmos::Simulator::set_current(&old_sim);
        tracked = malloc(8);
        assert(tracked != nullptr);
        strcpy(static_cast<char*>(tracked), "cosmos");
        cosmos::Simulator::set_current(nullptr);
    }

    cosmos::Simulator fresh_sim;
    cosmos::Simulator::set_current(&fresh_sim);

    auto* grown = static_cast<char*>(realloc(tracked, 256));
    assert(grown != nullptr);
    assert(strcmp(grown, "cosmos") == 0);
    assert(fresh_sim.heap().stats().active_allocations == 1);

    free(grown);
    assert(fresh_sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_realloc_after_sim_destruction_with_new_sim" << std::endl;
}

// Same emulation with no simulator active at all: the fresh block comes from the host allocator
// and the old orphaned block is released without touching any dead heap.
void test_realloc_after_sim_destruction_without_sim() {
    void* tracked = nullptr;
    {
        cosmos::Simulator sim;
        cosmos::Simulator::set_current(&sim);
        tracked = malloc(16);
        assert(tracked != nullptr);
        strcpy(static_cast<char*>(tracked), "sim");
        cosmos::Simulator::set_current(nullptr);
    }
    assert(!cosmos::Simulator::has_current());

    auto* grown = static_cast<char*>(realloc(tracked, 512));
    assert(grown != nullptr);
    assert(strcmp(grown, "sim") == 0);

    free(grown);

    std::cout << "[PASS] test_realloc_after_sim_destruction_without_sim" << std::endl;
}

// Regression: releasing an orphaned block must also drop its registry entry. A stale Orphaned
// entry would outlive the freed block and misroute a later passthrough allocation that reuses
// the payload address, turning that block's free into a header read of a live host allocation.
void test_freed_orphan_leaves_no_registry_entry() {
    void* tracked = nullptr;
    {
        cosmos::Simulator sim;
        cosmos::Simulator::set_current(&sim);
        tracked = malloc(48);
        assert(tracked != nullptr);
        cosmos::Simulator::set_current(nullptr);
    }
    assert(!cosmos::Simulator::has_current());

    auto& registry = cosmos::detail::AllocRegistry::instance();
    assert(registry.ownership_of(tracked).kind == cosmos::detail::OwnerKind::Orphaned);

    free(tracked);
    assert(registry.ownership_of(tracked).kind == cosmos::detail::OwnerKind::None);

    std::cout << "[PASS] test_freed_orphan_leaves_no_registry_entry" << std::endl;
}

// After an orphaned block is released, the host allocator may hand out any address from the
// freed chunk, including split fragments. None of those passthrough blocks may ever be
// classified as Orphaned, and each must free cleanly.
void test_passthrough_blocks_survive_after_orphan_release() {
    void* tracked = nullptr;
    {
        cosmos::Simulator sim;
        cosmos::Simulator::set_current(&sim);
        tracked = malloc(48);
        assert(tracked != nullptr);
        cosmos::Simulator::set_current(nullptr);
    }
    free(tracked);

    auto& registry = cosmos::detail::AllocRegistry::instance();
    assert(registry.ownership_of(tracked).kind == cosmos::detail::OwnerKind::None);

    // The first size matches the orphan's raw block so the allocator reuses its chunk; the
    // small sizes then force split fragments at offsets inside the old layout.
    const std::size_t sizes[] = {80, 1, 8, 24, 40, 48, 80, 96, 128};
    for (std::size_t size : sizes) {
        void* q = malloc(size);
        assert(q != nullptr);
        assert(registry.ownership_of(q).kind != cosmos::detail::OwnerKind::Orphaned);
        memset(q, 0x5A, size);
        free(q);
    }

    std::cout << "[PASS] test_passthrough_blocks_survive_after_orphan_release" << std::endl;
}

// realloc of an orphaned block releases the old block through free_orphaned_block, so the old
// pointer's registry entry must be gone afterwards; the fresh block is tracked by the active heap.
void test_orphan_realloc_releases_registry_entry() {
    void* tracked = nullptr;
    {
        cosmos::Simulator old_sim;
        cosmos::Simulator::set_current(&old_sim);
        tracked = malloc(24);
        assert(tracked != nullptr);
        strcpy(static_cast<char*>(tracked), "orphan");
        cosmos::Simulator::set_current(nullptr);
    }

    auto& registry = cosmos::detail::AllocRegistry::instance();
    assert(registry.ownership_of(tracked).kind == cosmos::detail::OwnerKind::Orphaned);

    cosmos::Simulator fresh_sim;
    cosmos::Simulator::set_current(&fresh_sim);

    auto* grown = static_cast<char*>(realloc(tracked, 256));
    assert(grown != nullptr);
    assert(strcmp(grown, "orphan") == 0);
    assert(registry.ownership_of(tracked).kind == cosmos::detail::OwnerKind::None);
    assert(registry.ownership_of(grown).kind == cosmos::detail::OwnerKind::Owned);
    assert(fresh_sim.heap().owns(grown));

    free(grown);
    assert(fresh_sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_orphan_realloc_releases_registry_entry" << std::endl;
}

// An intermediate oom_rate draws one decision per sim-heap allocation from the Memory fault
// sub-stream: same seed => identical inject/skip pattern, and the pattern actually contains both
// outcomes.
void test_intermediate_oom_rate_injects_deterministically() {
    constexpr int kAllocations = 100;

    auto run = [](uint64_t seed) {
        cosmos::Simulator sim(seed);
        cosmos::FaultProfile fp;
        fp.oom_rate = 0.5;
        sim.set_faults(fp);
        cosmos::Simulator::set_current(&sim);

        size_t failures = 0;
        for (int i = 0; i < kAllocations; ++i) {
            void* p = malloc(16);
            if (p) {
                free(p);
            } else {
                ++failures;
            }
        }

        cosmos::Simulator::set_current(nullptr);
        return failures;
    };

    const size_t first = run(1234);
    const size_t second = run(1234);
    assert(first == second);
    assert(first > 0);            // intermediate rate does inject
    assert(first < kAllocations); // and does not inject everything

    std::cout << "[PASS] test_intermediate_oom_rate_injects_deterministically" << std::endl;
}

int main() {
    test_passthrough_malloc();
    test_active_sim_malloc_and_alignment();
    test_oom_fault_injection();
    test_zero_byte_allocation();
    test_reentrancy_and_many_allocations();
    test_calloc_zero_init_and_tracking();
    test_calloc_overflow_rejected();
    test_calloc_oom_fault_injection();
    test_realloc_null_behaves_as_malloc();
    test_realloc_grow_and_shrink_preserve_data();
    test_realloc_zero_size_frees();
    test_realloc_oom_leaves_original_intact();
    test_realloc_passthrough_pointer();
    test_free_passthrough_pointer_inside_sim();
    test_free_after_sim_deactivation_routes_to_owner();
    test_free_under_foreign_sim_updates_original_owner();
    test_realloc_after_sim_deactivation_routes_to_owner();
    test_realloc_under_foreign_sim_routes_to_owner();
    test_free_after_sim_destruction_orphans_block();
    test_realloc_after_sim_destruction_with_new_sim();
    test_realloc_after_sim_destruction_without_sim();
    test_freed_orphan_leaves_no_registry_entry();
    test_passthrough_blocks_survive_after_orphan_release();
    test_orphan_realloc_releases_registry_entry();
    test_intermediate_oom_rate_injects_deterministically();
    std::cout << "All malloc wrapper tests passed successfully!" << std::endl;
    return 0;
}
