# Writing a Wrapper

How to add a POSIX function to the interposition layer so it satisfies the invariants in
[`architecture.md`](architecture.md#6-invariants) and the rules in
[`determinism-contract.md`](determinism-contract.md).

Read [`IMPLEMENTATION_NOTES.md`](IMPLEMENTATION_NOTES.md) first if you have not yet seen what
`--wrap` can and cannot reach.

---

## 1. The checklist

Every wrapper is the same shape. In order:

1. **One translation unit per family.** Same-TU calls bypass the wrap, so the definition must not
   share a TU with its callers. Existing families: `wrap_memory.cpp`, `wrap_time.cpp`,
   `wrap_random.cpp`, `wrap_storage.cpp`, `wrap_threads.cpp`, `wrap_net.cpp`.
2. **Declare `__real_X`** in a local `extern "C"` block, and call it — never the bare symbol — on
   every passthrough path. Calling the bare symbol recurses forever.
3. **Early exit 1: no universe.** `Simulator::current() == nullptr` means this call arrived with
   no active universe, including from a second OS thread. Passthrough to `__real_X`, touch no
   engine state, leave `errno` alone. This is [I2](architecture.md#6-invariants).
4. **Early exit 2: reentrancy.** If `wrappers::in_wrapper_logic` is already set, this is engine
   work. Skip eligibility and skip `decide` entirely. Use `wrappers::ReentrancyGuard` to set it
   around your own logic. This is [I3](architecture.md#6-invariants).
5. **Eligibility predicate.** Decide whether this call is a legal place for a fault to be
   observable at all (Rule 15). If not, do the real work and return — **and do not draw**.
   Drawing on an ineligible call violates Rule 3 and changes every later seed.
6. **One decision.** `decide_for(sim, class, site)` exactly once per eligible call.
7. **Translate to a legal result.** Never invent an impossible world.
8. **Register the site.** Append a `SiteId` (and `FaultKind` if you need a new outcome) — never
   renumber (Rule 13).
9. **Add the `-Wl,--wrap=X` flag to every consumer target.** Forgetting this is silent.
10. **Test it.** See [`testing.md`](testing.md).

---

## 2. Skeleton

```cpp
// wrap_foo.cpp — one family per TU.
#include "wrapper_fault.hpp"
#include "cosmos/simulator.hpp"

extern "C" {
ssize_t __real_foo(int fd, const void* buf, size_t count);
}

extern "C" ssize_t __wrap_foo(int fd, const void* buf, size_t count) {
    // (3) No universe -> passthrough. Nothing engine-side is touched.
    auto* sim = cosmos::Simulator::current();
    if (sim == nullptr) return __real_foo(fd, buf, count);

    // (4) Engine work -> never faulted.
    cosmos::wrappers::ReentrancyGuard guard;

    // (5) Ineligible -> real work, no draw.
    if (!cosmos::wrappers::foo_eligible(fd, count)) {
        return __real_foo(fd, buf, count);
    }

    // (6) One decision per eligible call.
    const cosmos::FaultKind kind =
        cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::foo);

    // (7) Legal outcomes only (Rule 15).
    switch (kind) {
    case cosmos::FaultKind::None:
        break;
    case cosmos::FaultKind::StorageEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::ShortWrite: {
        // An honest partial transfer, not a claimed one.
        const size_t half = count / 2;
        return __real_foo(fd, buf, half);
    }
    default:
        break; // an outcome illegal for this site is a Rule 15 violation
    }

    return __real_foo(fd, buf, count);
}
```

The eligibility predicate belongs next to the others in `wrapper_fault.hpp`, and it must be
`constexpr` and allocation-free ([I5](architecture.md#6-invariants)).

---

## 3. What the invariants mean in practice

### Order of the two early exits

Swap them and you either draw for a call that has no universe, or fault the engine's own work.
Both silently change every later seed. [§7.4 of `architecture.md`](architecture.md#7-sequence-diagrams)
draws this path.

### Eligibility is not "should we inject"

It is "could an outcome here be legal and observable at all". The existing predicates show the
distinction:

| Predicate | Rule |
|---|---|
| `memory_alloc_eligible(size)` → always `true` | `malloc(0)` returning `nullptr` is legal C11, so a fire there is observable |
| `memory_calloc_eligible(n, s)` | A size-product overflow is a real API failure, not a fault — answering it before the injector keeps it from consuming a draw |
| `memory_realloc_eligible(p, s, owned)` | `realloc(ptr, 0)` is a free; a block this universe does not own is not its call to fault |
| `storage_fd_eligible(fd)` → `fd > 2` | Standard streams are excluded so logging cannot consume Storage draws or fail with injected errors |
| `storage_write_eligible(fd, count)` | An empty transfer has nothing to observe; a 1-byte write stays eligible |

### Translating a fault to a result

Rule 15 is strict, and it is easy to get subtly wrong. `ShortWrite` is the worked example: a
`write()` reporting *k* bytes must mean *k* bytes were transferred. Claiming a short count without
transferring is an impossible world, so `wrap_storage.cpp` performs a **real** half-length
`write()`. The degenerate case — a fired `ShortWrite` on a 1-byte write — has no legal short
observable and observably degrades to a complete write.

Ask before inventing a result: *could a real system produce exactly this?*

### Stream discipline

Draw only when the decision is actually made (Rule 3), only from the site's own class stream
(Rule 2), and never from anything but the `fault` stream (Rule 1). `decide_for` handles this; do
not call the RNG directly in a wrapper.

### No allocation, ever

A wrapper that allocates re-enters `__wrap_malloc`. Engine metadata uses `RawRealAllocator`
(`memory.hpp`) to reach `__real_malloc` past the wrapper; if you need a container inside the
engine, use it.

---

## 4. Registering the site

In `faults.hpp`:

1. **Append** to `SiteId`. Never insert in the middle, never renumber, never reuse a value
   (Rule 13). The `-Werror=switch` build plus the `site_slot()` consistency proof exist to make
   this safe.
2. Add the site to `class_of()` so its fault class is derivable.
3. If the outcome needs a new `FaultKind`, append it there too.
4. Add it to `is_legal_outcome()` — the legality matrix that Rule 15 enforces.
5. Add it to `site_slot()` and let the compile-time assertion check the numbering.

If you add an outcome, check `widest_legal_menu()`: `OutcomeTable`'s capacity is `static_assert`ed
against it, so widening a menu may widen the table.

Then make sure the site's class is one the wrapper actually consults. A site that validates but
can never fire violates [I10](architecture.md#6-invariants) — see
[DEF-8](roadmap.md#known-defects) and [DEF-9](roadmap.md#known-defects) for what that costs.

---

## 5. Build wiring

Add `-Wl,--wrap=X` to **every** consumer target that should interpose it:

- `tests/CMakeLists.txt` — each test target carries its own list.
- `examples/single_node/CMakeLists.txt` and `examples/distributed/CMakeLists.txt` — currently
  34 flags each.

And confirm the three subset-link flags are still in force
([I6](architecture.md#6-invariants)): `-ffunction-sections`/`-fdata-sections` on `libcosmos`,
`-Wl,--gc-sections` on the consumer, and `-fno-builtin-X` if the compiler has a builtin for it.

`-fno-builtin-X` matters specifically when the function has a compiler builtin. `malloc`, `free`,
`calloc`, `realloc`, `random`, `rand`, `srand`, `srandom` all do. Adding a wrapped function with a
builtin and forgetting the flag produces a wrapper that exists and is never called.

---

## 6. Common failures

| Symptom | Cause |
|---|---|
| Wrapper never called | Missing `-Wl,--wrap=X` on that target, or a builtin was folded back (`-fno-builtin-X` missing) |
| Wrapper never called for `read` | glibc substituted `__read_chk`; you need `__wrap___read_chk` too |
| Infinite recursion in the wrapper | Called the bare symbol instead of `__real_X`, or allocated without `RawRealAllocator` |
| Later seeds differ after adding the wrapper | Drew on an ineligible call (Rule 3), or drew more than once, or drew from the wrong stream |
| `std::` container allocations not faulted | Shared `libstdc++` link — see [`determinism-contract.md`](determinism-contract.md) §4 |
| Fault fires in engine internals | Missing `ReentrancyGuard` — [DEF-14](roadmap.md#known-defects) is this bug in three existing families |
| Config validates but nothing happens | Site added to `validate()`'s accepted set but no wrapper consults the injector — [I10](architecture.md#6-invariants) |
