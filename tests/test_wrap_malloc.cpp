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
    std::cout << "All malloc wrapper tests passed successfully!" << std::endl;
    return 0;
}
