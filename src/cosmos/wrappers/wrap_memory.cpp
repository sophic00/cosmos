#include "cosmos/cosmos.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
thread_local bool in_wrap_memory = false;

struct ReentrancyGuard {
    ReentrancyGuard() { in_wrap_memory = true; }
    ~ReentrancyGuard() { in_wrap_memory = false; }
};
} // namespace

extern "C" {

void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __real_calloc(size_t nmemb, size_t size);
void* __real_realloc(void* ptr, size_t size);

/**
 * @brief Linker interposition wrapper for standard C `malloc(size)`.
 *
 * WHAT IT IS:
 * Intercepts all calls to standard C `malloc` at final link time when compiled with
 * `-Wl,--wrap=malloc`.
 *
 * WHY & WHEN IT IS USED:
 * - Used during testing builds (`-DCOSMOS_SIM`) to capture memory allocation calls in application
 * code and linked libraries.
 * - Instantiates `ReentrancyGuard` immediately after checking `in_wrap_memory` to ensure all
 * subsequent operations
 *   (`Simulator::has_current()`, `faults().should_inject_oom()`, `heap().record_oom()`, and
 * `heap().allocate(size)`) are fully protected from recursive re-entrancy loops if any internal
 * operation calls `malloc`.
 * - When an active `Simulator` context (`Simulator::has_current()`) is running:
 *   1. Checks deterministic OOM fault injection (`sim->faults().should_inject_oom()`). If
 * triggered, sets `errno = ENOMEM` and returns `nullptr`.
 *   2. Delegates to `sim->heap().allocate(size)` to attach allocation metadata headers and record
 * stats.
 * - Outside an active simulation context or during internal re-entrant allocations
 * (`in_wrap_memory == true`), falls back directly to native OS `__real_malloc(size)`.
 */
void* __wrap_malloc(size_t size) {
    if (in_wrap_memory) {
        return __real_malloc(size);
    }

    ReentrancyGuard guard;

    if (!cosmos::Simulator::has_current()) {
        return __real_malloc(size);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->faults().should_inject_oom()) {
        errno = ENOMEM;
        sim->heap().record_oom();
        return nullptr;
    }

    return sim->heap().allocate(size);
}

/**
 * @brief Linker interposition wrapper for standard C `free(ptr)`.
 *
 * WHAT IT IS:
 * Intercepts all calls to standard C `free` at final link time when compiled with
 * `-Wl,--wrap=free`.
 *
 * WHY & WHEN IT IS USED:
 * - Used during testing builds (`-DCOSMOS_SIM`) to capture heap deallocation calls.
 * - Immediately returns if `ptr == nullptr`.
 * - Instantiates `ReentrancyGuard` to protect context lookups and `TrackedHeap::deallocate` from
 * recursive re-entrancy loops.
 * - If inside an active `Simulator` context (`Simulator::has_current()`):
 *   Delegates to `sim->heap().deallocate(ptr)`. If `deallocate` recognizes the pointer's header
 * canary magic, it updates active heap statistics, marks the header as freed, and frees the raw
 * header via `__real_free`.
 * - If no current Simulator owns the pointer (the owning Simulator may no longer be current), the
 * canary is checked directly: handing a tracked payload to `__real_free` would free an address one
 * header past the real allocation, so the tracked block is freed via its header instead.
 * - For passthrough allocations (allocated via `__real_malloc` outside a simulation context) or
 * during re-entrancy, falls back directly to native OS `__real_free(ptr)`.
 */
void __wrap_free(void* ptr) {
    if (!ptr) return;

    if (in_wrap_memory) {
        __real_free(ptr);
        return;
    }

    ReentrancyGuard guard;

    if (cosmos::Simulator::has_current()) {
        auto* sim = cosmos::Simulator::current();
        if (sim->heap().deallocate(ptr)) {
            return;
        }
    }

    // The owning Simulator may no longer be current, so the canary is checked here too: handing a
    // tracked payload to __real_free would free an address one header past the real allocation.
    auto* header = cosmos::header_for(ptr);
    if (header->magic == cosmos::COSMOS_CANARY_MAGIC) {
        header->magic = cosmos::COSMOS_FREED_MAGIC;
        __real_free(header);
        return;
    }

    // Passthrough allocation (allocated via __real_malloc outside simulation context)
    __real_free(ptr);
}

/**
 * @brief Linker interposition wrapper for standard C `calloc(nmemb, size)`.
 *
 * WHAT IT IS:
 * Intercepts all calls to standard C `calloc` at final link time when compiled with
 * `-Wl,--wrap=calloc`.
 *
 * WHY & WHEN IT IS USED:
 * - Used during testing builds (`-DCOSMOS_SIM`) to capture zero-initialized heap allocation calls
 * in application code and linked libraries.
 * - Instantiates `ReentrancyGuard` (same re-entrancy protection as `__wrap_malloc`).
 * - Inside an active `Simulator` context:
 *   1. Rejects `nmemb * size` overflow with `errno = ENOMEM` before any allocation. This is a
 *      legitimate API failure, not fault injection, so `oom_fault_count` is not incremented.
 *   2. Applies the same deterministic OOM fault injection as `__wrap_malloc` (`errno = ENOMEM`,
 *      `heap().record_oom()`, return `nullptr`).
 *   3. Delegates to `sim->heap().allocate(nmemb * size)` and zero-fills the tracked payload, so
 *      the allocation participates in leak detection and stats like any other sim-heap block.
 * - Outside an active simulation context or during re-entrancy, falls back to native OS
 * `__real_calloc(nmemb, size)`.
 */
void* __wrap_calloc(size_t nmemb, size_t size) {
    if (in_wrap_memory) {
        return __real_calloc(nmemb, size);
    }

    ReentrancyGuard guard;

    if (!cosmos::Simulator::has_current()) {
        return __real_calloc(nmemb, size);
    }

    auto* sim = cosmos::Simulator::current();

    if (nmemb != 0 && size > (SIZE_MAX / nmemb)) {
        errno = ENOMEM;
        return nullptr;
    }

    if (sim->faults().should_inject_oom()) {
        errno = ENOMEM;
        sim->heap().record_oom();
        return nullptr;
    }

    size_t total_size = nmemb * size;
    void* ptr = sim->heap().allocate(total_size);
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

/**
 * @brief Linker interposition wrapper for standard C `realloc(ptr, size)`.
 *
 * WHAT IT IS:
 * Intercepts all calls to standard C `realloc` at final link time when compiled with
 * `-Wl,--wrap=realloc`.
 *
 * WHY & WHEN IT IS USED:
 * - Used during testing builds (`-DCOSMOS_SIM`) to keep resize operations inside the tracked sim
 * heap instead of silently escaping to the host allocator.
 * - Instantiates `ReentrancyGuard` (same re-entrancy protection as `__wrap_malloc`).
 * - Inside an active `Simulator` context:
 *   1. `realloc(ptr, 0)` frees `ptr` (tracked or passthrough) and returns `nullptr`,
 *      glibc-compatible.
 *   2. `realloc(nullptr, size)` behaves exactly like `__wrap_malloc(size)`, including OOM fault
 *      injection.
 *   3. For pointers owned by the sim heap: OOM injection may fail the resize with
 *      `errno = ENOMEM`; the original block then remains valid and tracked (standard realloc
 *      guarantee). Otherwise `heap().reallocate` allocates a fresh tracked block, copies
 *      `min(old, new)` bytes, and frees the old block. OOM injection only applies to allocations
 *      served by the sim heap; passthrough pointers never consume fault-stream decisions.
 *   4. For passthrough pointers (allocated via `__real_malloc` outside the simulation context),
 *      forwards to native OS `__real_realloc`.
 */
void* __wrap_realloc(void* ptr, size_t size) {
    if (in_wrap_memory) {
        return __real_realloc(ptr, size);
    }

    ReentrancyGuard guard;

    if (!cosmos::Simulator::has_current()) {
        return __real_realloc(ptr, size);
    }

    auto* sim = cosmos::Simulator::current();

    if (size == 0) {
        if (ptr) {
            if (!sim->heap().deallocate(ptr)) {
                __real_free(ptr);
            }
        }
        return nullptr;
    }

    if (!ptr) {
        if (sim->faults().should_inject_oom()) {
            errno = ENOMEM;
            sim->heap().record_oom();
            return nullptr;
        }
        return sim->heap().allocate(size);
    }

    if (sim->heap().owns(ptr)) {
        if (sim->faults().should_inject_oom()) {
            errno = ENOMEM;
            sim->heap().record_oom();
            return nullptr;
        }
        void* new_ptr = sim->heap().reallocate(ptr, size);
        if (!new_ptr) {
            errno = ENOMEM;
        }
        return new_ptr;
    }

    // Passthrough allocation (allocated via __real_malloc outside simulation context)
    return __real_realloc(ptr, size);
}

} // extern "C"
