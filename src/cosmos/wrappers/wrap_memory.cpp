#include "cosmos/cosmos.hpp"

#include "wrapper_fault.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Releases a block whose owning heap died first. The registry entry is dropped only on success: a
// stale one would misroute a later passthrough allocation that reuses the address.
void free_orphaned_block(void* ptr) {
    auto* header = cosmos::header_for(ptr);
    if (header->magic == cosmos::COSMOS_CANARY_MAGIC) {
        header->magic = cosmos::COSMOS_FREED_MAGIC;
        __real_free(header);
        cosmos::detail::AllocRegistry::instance().unregister(ptr);
    }
}

// Never faulted: the block never belonged to the currently active universe.
void* reallocate_orphaned_block(void* ptr, size_t new_size, bool engine_call) {
    auto* header = cosmos::header_for(ptr);
    if (header->magic != cosmos::COSMOS_CANARY_MAGIC) {
        errno = ENOMEM;
        return nullptr;
    }

    const size_t old_size = header->requested_size;
    // Engine work must not pollute the sim heap (mirrors malloc/calloc passthrough):
    // a tracked orphan reallocated by the engine becomes an untracked host block.
    // engine_call is sampled before the wrapper's guard (which is held here), so it
    // must be passed in rather than re-reading in_wrapper_logic.
    void* new_ptr = (cosmos::Simulator::has_current() && !engine_call)
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

bool decided_oom(cosmos::Simulator* sim, cosmos::SiteId site) {
    return cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Memory, site) ==
           cosmos::FaultKind::OutOfMemory;
}

} // namespace

extern "C" {

void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __real_calloc(size_t nmemb, size_t size);
void* __real_realloc(void* ptr, size_t size);

void* __wrap_malloc(size_t size) {
    if (cosmos::wrappers::in_wrapper_logic || !cosmos::Simulator::has_current()) {
        return __real_malloc(size);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    // Decided before the heap is touched, so a fired OOM leaves TrackedHeap exactly as it was.
    if (cosmos::wrappers::memory_alloc_eligible(size) && decided_oom(sim, cosmos::SiteId::malloc)) {
        errno = ENOMEM;
        return nullptr;
    }

    return sim->heap().allocate(size);
}

void __wrap_free(void* ptr) {
    if (!ptr) return;
    cosmos::wrappers::ReentrancyGuard guard;

    // Ownership-based, not context-based (Rule 7): an engine call that frees an application block
    // must still reach that block's heap. Handing a sim payload to __real_free would release the
    // address header_size past the block and corrupt the host allocator instead.
    auto ownership = cosmos::detail::AllocRegistry::instance().ownership_of(ptr);
    if (ownership.kind == cosmos::detail::OwnerKind::Owned) {
        ownership.owner->deallocate(ptr);
        return;
    }
    if (ownership.kind == cosmos::detail::OwnerKind::Orphaned) {
        free_orphaned_block(ptr);
        return;
    }

    __real_free(ptr);
}

void* __wrap_calloc(size_t nmemb, size_t size) {
    if (cosmos::wrappers::in_wrapper_logic || !cosmos::Simulator::has_current()) {
        return __real_calloc(nmemb, size);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();

    if (!cosmos::wrappers::memory_calloc_eligible(nmemb, size)) {
        errno = ENOMEM;
        return nullptr;
    }

    if (decided_oom(sim, cosmos::SiteId::calloc)) {
        errno = ENOMEM;
        return nullptr;
    }

    const size_t total_size = nmemb * size;
    void* ptr = sim->heap().allocate(total_size);
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void* __wrap_realloc(void* ptr, size_t size) {
    // Rule 7: an engine call must not spend a Memory draw, but it must still route by ownership --
    // a tracked payload handed to __real_realloc would be reallocated from its header offset.
    const bool engine_call = cosmos::wrappers::in_wrapper_logic;
    cosmos::wrappers::ReentrancyGuard guard;

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
        if (!cosmos::Simulator::has_current() || engine_call) {
            return __real_malloc(size);
        }
        auto* sim = cosmos::Simulator::current();
        if (cosmos::wrappers::memory_realloc_eligible(ptr, size, /*owned_by_sim=*/false) &&
            decided_oom(sim, cosmos::SiteId::realloc)) {
            errno = ENOMEM;
            return nullptr;
        }
        return sim->heap().allocate(size);
    }

    if (ownership.kind == cosmos::detail::OwnerKind::Owned) {
        if (!engine_call && cosmos::Simulator::has_current()) {
            auto* sim = cosmos::Simulator::current();
            if (cosmos::wrappers::memory_realloc_eligible(ptr, size, sim->heap().owns(ptr)) &&
                decided_oom(sim, cosmos::SiteId::realloc)) {
                errno = ENOMEM;
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
        return reallocate_orphaned_block(ptr, size, engine_call);
    }

    return __real_realloc(ptr, size);
}

} // extern "C"
