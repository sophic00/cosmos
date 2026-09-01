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

// Frees a block whose owning heap was destroyed while the block was still allocated. Registry
// membership proves the pointer is a sim payload, so the header read is safe. A corrupted canary
// leaks the block rather than freeing a bogus header-offset address; the registry entry stays in
// that case, so any later free of the same pointer takes this same header-verified path instead of
// a passthrough __real_free of a header-offset address. On a successful release the entry is
// removed: a stale Orphaned entry would outlive the freed block and misroute a later passthrough
// allocation that reuses the address.
void free_orphaned_block(void* ptr) {
    auto* header = cosmos::header_for(ptr);
    if (header->magic == cosmos::COSMOS_CANARY_MAGIC) {
        header->magic = cosmos::COSMOS_FREED_MAGIC;
        __real_free(header);
        cosmos::detail::AllocRegistry::instance().unregister(ptr);
    }
}

// realloc semantics for a block whose owning heap is gone: fresh allocation (through the active
// sim heap when one exists), copy min(old, new), release the old block. No fault injection: the
// block never belonged to the currently active universe. A corrupted canary fails the resize and
// leaves the original untouched.
void* reallocate_orphaned_block(void* ptr, size_t new_size) {
    auto* header = cosmos::header_for(ptr);
    if (header->magic != cosmos::COSMOS_CANARY_MAGIC) {
        errno = ENOMEM;
        return nullptr;
    }

    const size_t old_size = header->requested_size;
    void* new_ptr = cosmos::Simulator::has_current()
                        ? cosmos::Simulator::current()->heap().allocate(new_size)
                        : __real_malloc(new_size);
    if (!new_ptr) {
        errno = ENOMEM;
        return nullptr;
    }

    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    free_orphaned_block(ptr);
    return new_ptr;
}
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
 * subsequent operations (`Simulator::has_current()`, `faults().should_inject_oom()`,
 * `heap().record_oom()`, and `heap().allocate(size)`) are fully protected from recursive
 * re-entrancy loops if any internal operation calls `malloc`.
 * - When an active `Simulator` context (`Simulator::has_current()`) is running:
 *   1. Checks deterministic OOM fault injection
 * (`sim->faults().should_inject_oom(sim->fault_rng())`, drawing from the Memory fault sub-stream).
 * If triggered, sets `errno = ENOMEM` and returns `nullptr`.
 *   2. Delegates to `sim->heap().allocate(size)` to attach allocation metadata headers and record
 *      stats.
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
    if (sim->faults().should_inject_oom(sim->fault_rng())) {
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
 * - Instantiates `ReentrancyGuard` to protect registry lookups and `TrackedHeap::deallocate` from
 *   recursive re-entrancy loops.
 * - Routing is ownership-based via `detail::AllocRegistry`, independent of which `Simulator` (if
 *   any) is currently active:
 *   1. Owned: the pointer belongs to a live `TrackedHeap`, which is handed the deallocation so
 *      the *owning* heap's stats and registry stay correct (cross-simulator frees update the
 *      right heap instead of leaking phantom active allocations).
 *   2. Orphaned: the owning heap was destroyed with the block still allocated. The block is
 *      released via its header, which the registry proves is a real sim payload.
 *   3. None: the pointer never came from a sim heap, so no header bytes are read and the
 *      passthrough pointer goes straight to `__real_free(ptr)`.
 * - During re-entrancy, falls back directly to native OS `__real_free(ptr)`.
 */
void __wrap_free(void* ptr) {
    if (!ptr) return;

    if (in_wrap_memory) {
        __real_free(ptr);
        return;
    }

    ReentrancyGuard guard;

    auto ownership = cosmos::detail::AllocRegistry::instance().ownership_of(ptr);
    if (ownership.kind == cosmos::detail::OwnerKind::Owned) {
        ownership.owner->deallocate(ptr);
        return;
    }
    if (ownership.kind == cosmos::detail::OwnerKind::Orphaned) {
        free_orphaned_block(ptr);
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

    if (sim->faults().should_inject_oom(sim->fault_rng())) {
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
 * - Routing is ownership-based via `detail::AllocRegistry`, independent of which `Simulator` (if
 *   any) is currently active:
 *   1. `realloc(ptr, 0)` frees `ptr` (owned -> owning heap, orphaned -> header-verified raw free,
 *      passthrough -> `__real_free`) and returns `nullptr`, glibc-compatible.
 *   2. `realloc(nullptr, size)` behaves exactly like `__wrap_malloc(size)`, including OOM fault
 *      injection.
 *   3. For pointers owned by a live sim heap: the resize is routed to the *owning* heap, so a
 *      reallocation from a foreign simulator context stays tracked and accounted by the right
 *      heap. OOM injection is decided by the currently active simulator's profile, and only when
 *      that simulator's heap owns the block, so passthrough and foreign-owned resizes never
 *      consume fault-stream decisions. On injected or real failure the original block remains
 *      valid and tracked (standard realloc guarantee); `heap().reallocate` otherwise allocates a
 *      fresh tracked block, copies `min(old, new)` bytes, and frees the old block.
 *   4. For orphaned pointers (owning heap destroyed with the block still allocated), the resize
 *      is emulated: fresh allocation, copy, header-verified release of the old block.
 *   5. For passthrough pointers (allocated via `__real_malloc` outside the simulation context),
 *      forwards to native OS `__real_realloc`. The registry miss proves the pointer is not a
 *      sim payload, so the real allocator never sees a header-offset address.
 */
void* __wrap_realloc(void* ptr, size_t size) {
    if (in_wrap_memory) {
        return __real_realloc(ptr, size);
    }

    ReentrancyGuard guard;

    auto ownership = cosmos::detail::AllocRegistry::instance().ownership_of(ptr);

    if (size == 0) {
        if (ownership.kind == cosmos::detail::OwnerKind::Owned) {
            ownership.owner->deallocate(ptr);
        } else if (ownership.kind == cosmos::detail::OwnerKind::Orphaned) {
            free_orphaned_block(ptr);
        } else if (ptr) {
            __real_free(ptr);
        }
        return nullptr;
    }

    if (!ptr) {
        if (!cosmos::Simulator::has_current()) {
            return __real_malloc(size);
        }
        auto* sim = cosmos::Simulator::current();
        if (sim->faults().should_inject_oom(sim->fault_rng())) {
            errno = ENOMEM;
            sim->heap().record_oom();
            return nullptr;
        }
        return sim->heap().allocate(size);
    }

    if (ownership.kind == cosmos::detail::OwnerKind::Owned) {
        if (cosmos::Simulator::has_current()) {
            auto* sim = cosmos::Simulator::current();
            if (sim->heap().owns(ptr) && sim->faults().should_inject_oom(sim->fault_rng())) {
                errno = ENOMEM;
                sim->heap().record_oom();
                return nullptr;
            }
        }
        void* new_ptr = ownership.owner->reallocate(ptr, size);
        if (!new_ptr) {
            errno = ENOMEM;
        }
        return new_ptr;
    }

    if (ownership.kind == cosmos::detail::OwnerKind::Orphaned) {
        return reallocate_orphaned_block(ptr, size);
    }

    // Passthrough allocation (allocated via __real_malloc outside simulation context)
    return __real_realloc(ptr, size);
}

} // extern "C"
