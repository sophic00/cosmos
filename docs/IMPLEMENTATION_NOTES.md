# Implementation Notes: What `-Wl,--wrap` Actually Reaches

This is the detailed boundary analysis of the interposition layer. The short version is in
[`architecture.md`](architecture.md#35-what-the-wrapper-mechanism-cannot-reach) and
[`determinism-contract.md`](determinism-contract.md) §5; this file is the reference.

`tests/CMakeLists.txt` refers to this document as "what `--wrap` actually reaches". This is it.

---

## 1. The mechanism

Cosmos uses GNU-ld symbol wrapping, not dynamic loading. There is no `dlsym`, no `RTLD_NEXT`, and
no `LD_PRELOAD` anywhere in the tree.

```cpp
extern "C" {
void* __real_malloc(size_t);          // supplied by the linker
void* __wrap_malloc(size_t);          // supplied by libcosmos
}
```

Passing `-Wl,--wrap=malloc` makes the linker resolve every undefined reference to `malloc` as
`__wrap_malloc`, and every undefined reference to `__real_malloc` as the real `malloc`. Both
directions are linker-side substitutions on undefined symbols.

This is why [`linker-interposition.md`](linker-interposition.md) §2 calls it *synthetic* symbol
resolution: `__real_malloc` is not a symbol that exists in libc. The linker invents it.

---

## 2. What it reaches

A wrapped symbol is reached when the call is an **undefined reference at link time** from an
object file that the linker processes. Concretely, all of these are reached:

- Calls from the application's own translation units.
- Calls from static libraries linked into the binary.
- Calls from C++ code the compiler emits, including `std::` container growth, **when**
  `libstdc++` is linked statically (see §4).
- Compiler-generated calls to functions with builtins, **when** `-fno-builtin-X` is in force.

---

## 3. What it does not reach

### 3.1 Same translation unit

If a call to `malloc` is resolved inside the same TU as its definition, no undefined reference is
emitted and the wrap never applies. This is the classic limitation.

**Mitigation in tree:** every `__wrap_*` lives in its own TU under `src/cosmos/wrappers/`, and no
application TU defines the wrapped symbol.

### 3.2 glibc fortify substitution

With `_FORTIFY_SOURCE`, glibc replaces `read(fd, buf, n)` with `__read_chk(fd, buf, n, buflen)`.
The compiler emits a call to `__read_chk`, which is a *different* symbol — so
`-Wl,--wrap=read` does nothing for it.

**Mitigation in tree:** `wrap_storage.cpp` defines `__wrap___read_chk` alongside `__wrap_read`,
and `tests/CMakeLists.txt` wraps both. The naming is unusual but correct: the wrap prefix attaches
to the full glibc symbol name.

This class of bug — a fortified variant that quietly bypasses the wrapper — is real and was found
by reading the code, not by a test. See the coverage gap noted in [`testing.md`](testing.md).

### 3.3 `operator new` and the `libstdc++` link mode

`operator new` calls `malloc`. But when `libstdc++` is linked **dynamically**, `operator new`
lives in `libstdc++.so`, and its internal call to `malloc` was already bound when that shared
object was built. `--wrap` rewrites undefined references in objects being linked *now*; it does
not reach inside an already-linked shared object.

So under the default link:

```
std::vector growth -> operator new (in libstdc++.so) -> malloc (bound there) -> glibc
                                                       ^ never __wrap_malloc
```

Under `-static-libstdc++`, `operator new` is pulled into the link as an ordinary object, its call
to `malloc` is an undefined reference, and `--wrap` rewrites it:

```
std::vector growth -> operator new -> __wrap_malloc -> TrackedHeap
```

**Consequence:** `fail_on_call = K` counts eligible calls at `SiteId::malloc`, and which
allocations reach that site is **linkage-dependent**. A test that needs an exact count must use
the raw allocator.

**Mitigation in tree:** `tests/CMakeLists.txt` links `test_scenario.cpp` twice — once normally as
`test_scenario`, once with `-static-libstdc++` as `test_scenario_static` — so both sides of the
behaviour are pinned rather than assumed. The static target is *probed* with
`check_cxx_source_compiles`, because some toolchains (including this repository's clang) have no
static `libstdc++` and an unconditional target would fail at link time rather than skip.

**Consequence in the clang tree:** `test_scenario_static` and `test_broken_app_static` are absent,
so the `operator new` interposition property is unverified there.

### 3.4 Raw syscalls

`syscall(SYS_write, fd, buf, n)` goes straight to the kernel. No wrapper, no fault, no virtual
time. This is not mitigated and is stated as a user obligation in
[`determinism-contract.md`](determinism-contract.md) §2.

### 3.5 Functions not wrapped at all

These fall through to the host with no diagnostic, even in a sim build:

| Family | Missing |
|---|---|
| I/O readiness | `epoll_*`, `poll`, `select`, `kqueue` |
| Message sockets | `sendmsg`, `recvmsg`, `sendto`, `recvfrom`, `writev`, `readv` |
| Name resolution | `getaddrinfo` and friends |
| Timers / signals | `timerfd_*`, `setitimer`, `timer_create`/`settime`, `sigaction`, `signalfd`, `signal` |
| Storage | `pread`, `pwrite`, `fdatasync`, `ftruncate`, `rename`, `unlink`, `mkdir`, `fcntl`, `flock`, `mmap`, `dup`, `dup2` |
| Threads | `pthread_cond_timedwait`, `pthread_mutex_timedlock`, rwlocks, barriers, semaphores, `pthread_once`, TLS keys, `pthread_attr_*` |

The thread row is the most dangerous, because it fails *quietly and destructively*: an unwrapped
pthread call operates on an object the scheduler believes it owns, using real glibc, on the
single OS thread. See [DEF-17](roadmap.md#known-defects) and
[DEF-23](roadmap.md#known-defects).

---

## 4. Subset wrapping

`libcosmos` defines more wrappers than any single target needs. Three flags cooperate so a target
ends up with exactly the set it asked for:

| Flag | Set where | Role |
|---|---|---|
| `-ffunction-sections -fdata-sections` | `src/cosmos/CMakeLists.txt` | Puts each `__wrap_*` in its own section |
| `-Wl,--gc-sections` | consumer link options | Drops sections nothing references |
| `-fno-builtin-X` | `src/cosmos/CMakeLists.txt`, `INTERFACE` | Prevents the optimiser folding a call before the linker sees it |

Without the first two, linking `libcosmos` drags in every wrapper. Without the third, a wrapper
can exist and never be called — the compiler turns `malloc(n)` into an intrinsic or folds
`random()` to a constant, and no undefined reference is emitted to rewrite.

The `INTERFACE` propagation of `-fno-builtin-X` matters: it means a consumer of `libcosmos`
cannot accidentally lose interposition by forgetting the flag.

Functions with compiler builtins in the current wrapper set: `malloc`, `free`, `calloc`,
`realloc`, `random`, `rand`, `srand`, `srandom`. A new wrapper for a function with a builtin needs
its own `-fno-builtin-X` entry.

---

## 5. `-DCOSMOS_PROD` / `-DCOSMOS_SIM` do nothing

The example targets set these as compile definitions. No source file reads them. The real switch
is the presence or absence of the `-Wl,--wrap=…` list on the target. They are documentation of
intent, not a mechanism. See [`architecture.md`](architecture.md#34-the-build-mode-defines-are-cosmetic).

---

## 6. Checking that a wrapper actually engaged

There is no automated check. A missed wrap flag is silent. Until one exists, verify by one of:

1. **Break it on purpose.** Temporarily make the wrapper return something absurd and confirm the
   test fails. If the test still passes, the wrap flag is missing or a builtin is being folded.
2. **`nm` the binary.** A wrapped symbol should show as `U __wrap_malloc`, not `U malloc`.
3. **Check the fortified variant.** If the function has a `*_chk` form, wrap both.

Item 1 is the only one that catches a `-fno-builtin` omission.

---

## 7. Engine-side escape hatches

Two mechanisms let the engine reach the host past its own wrappers. Both are invariants.

| Mechanism | Where | Purpose |
|---|---|---|
| `__real_*` | every `wrap_*.cpp` | Passthrough for calls with no universe, and for the real work behind a faultable call |
| `RawRealAllocator` | `memory.hpp` | Container allocations inside `TrackedHeap` go to `__real_malloc`, avoiding recursion in `__wrap_malloc` |
| `ReentrancyGuard` / `in_wrapper_logic` | `wrapper_fault.hpp` | Marks engine work so an arriving wrapped call passes through unfaulted |

These are [I2](architecture.md#6-invariants), [I3](architecture.md#6-invariants), and
[I5](architecture.md#6-invariants). See [`architecture.md`](architecture.md#6-invariants) for
their compliance status.
