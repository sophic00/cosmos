# The Determinism Contract

This is the normative statement of what Cosmos guarantees, what the application must not do, and
exactly where the guarantee stops. It is the single home for the determinism rules — the
rationale behind each lives in [`fault-injection.md`](fault-injection.md) §7, which links here
rather than restating them.

**Read this before blaming the engine for a non-reproducible failure.**

---

## 1. The guarantee

> For a fixed binary, a fixed build type, and a fixed 64-bit seed, a Cosmos simulation run is
> reproducible: the same calls return the same values at the same virtual timestamps, and the same
> faults fire at the same eligible occurrences.

Each qualifier below is load-bearing. Removing one makes the statement false.

| Qualifier | Why | If you need to relax it |
|---|---|---|
| **Fixed binary** | The draw count depends on which wrappers are linked ([I6](architecture.md#6-invariants)) and on inlining decisions | Rebuild both runs identically |
| **Fixed build type** | Three header asserts erase under `NDEBUG` — [DEF-3](roadmap.md#known-defects) | Not relaxable today |
| **Fixed compiler + flags** | Outcome selection accumulates doubles; FMA and `-ffp-contract` can move a bucket boundary — [DEF-18](roadmap.md#known-defects) | Not relaxable today |
| **Fixed seed** | The seed is the whole input | — |

**What is not guaranteed:** bit-identical replay across compilers, across optimisation levels, or
across Debug/Release. Same-compiler, same-flags replay is reliable and is what every test in the
suite relies on.

There is no trace hash in the tree, so none of this is externally verifiable today: a
reproducibility claim is currently proved only by running twice and comparing, which is what
`test_wrap_malloc`, `test_scenario`, and `test_broken_app` do. Trace recording and an
FNV-1a hash are [F5](roadmap.md#unified-roadmap).

---

## 2. User obligations

Determinism is a joint property. The engine holds up its end provided the application does too.

1. **Time** is read only through POSIX time functions (`clock_gettime`, `gettimeofday`) or the
   simulator's clock. Never from `rdtsc`, `RDTSCP`, or an inlined cycle counter.
2. **Randomness** is drawn only through POSIX random functions (`getrandom`, `random`, `rand`) or
   the simulator's user RNG. Never from `/dev/urandom` directly, from ASLR-derived values, or
   from `std::random_device`.
3. **I/O** goes only through POSIX socket and file calls.
4. **No iteration order depends on raw pointer values.** `std::unordered_map<T*>` and friends
   iterate in an address-dependent order, and addresses vary with ASLR. This is the single most
   common source of non-reproducibility.
5. **No raw syscalls.** `syscall(SYS_write, ...)` bypasses interposition entirely. This is not
   mitigated; see [`IMPLEMENTATION_NOTES.md`](IMPLEMENTATION_NOTES.md) for the boundary in
   detail.
6. **No reliance on real thread preemption.** Scheduling is cooperative. A program that only
   works because the kernel happened to preempt at the right instruction is not testable here.

Violating any of these produces a run that is non-reproducible *and* gives no diagnostic. There
is no trace hash to catch it.

---

## 3. Determinism rules

Normative. Rationale and worked examples are in [`fault-injection.md`](fault-injection.md) §7
(Rules 1-4) and §13 (the rest).

| # | Rule | What breaks if violated |
|---|---|---|
| 1 | Faults draw only from the `fault` stream | Changing fault rates silently changes thread interleavings |
| 2 | Each fault class gets its own sub-stream | Changing network settings shifts memory faults; impossible to vary one thing at a time |
| 3 | Never draw **at runtime** for a fault that cannot fire | Turning a fault *off* changes unrelated results |
| 4 | Never draw inside an address-order loop | ASLR leaks non-determinism back in |
| 5 | Episode faults always schedule their own heal; only persistent faults may omit one | Recovery becomes untestable, or a missing heal cannot be told from an intended one |
| 6 | Fault identity comes from the recorded decision trace, not a live counter | Minimization cannot be built |
| 7 | Engine-internal allocations are never faulted | The simulator corrupts itself; all results invalid |
| 8 | Config sampling uses a fixed class-indexed draw schedule, including disabled classes | Swarm dimensions become correlated; toggling one class shifts another's rate |
| 9 | Every recorded decision carries a status and a `drew` flag. The **ledger** records fires, heals and limit-refusals; the **decision trace** is the every-decision record | Replay cannot distinguish "returned early" from "drew and lost" |
| 10 | The trace hash covers a canonical encoding, never in-memory layout | Verification fails across compilers for non-determinism reasons |
| 11 | A deterministic trigger firing never consumes a draw | A scripted scenario perturbs unrelated probabilistic faults in the same run |
| 12 | Replay of a fixed decision trace never invents new faults: on mismatch or exhaustion, eligible calls pass through with no draw | Minimization re-runs get confounded by faults that never existed in the original |
| 13 | `SiteId` and `KnobId` are append-only; IDs are never renumbered or reused | Historical repro commands and ledgers silently re-point at the wrong sites |
| 14 | Knob values are sampled once per universe, delivered before the app starts, never mutated mid-run | A mid-run knob change is an unrecorded episode fault the ledger cannot explain |
| 15 | An injected result must be legal for its API, including invariants (e.g. `CLOCK_MONOTONIC` never steps backward) | The app is tested against an impossible world; findings are false positives |

Rule 13 is the reason `faults.hpp` carries a compile-time proof that `site_slot()` and the
`SiteId` enumeration agree, and why the build enables `-Werror=switch`: adding a site must not
renumber an existing one.

---

## 4. Build rules

Determinism is partly a property of the build. These flags are not optional.

| Rule | Where enforced | What breaks without it |
|---|---|---|
| `-fno-builtin-{malloc,free,calloc,realloc,random,rand,srand,srandom}` | `src/cosmos/CMakeLists.txt`, as an `INTERFACE` option | The optimiser folds allocations and random calls into intrinsics that never reach the wrapped symbol |
| `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` | `src/cosmos/CMakeLists.txt`, consumer link options | Unused wrappers are retained, or used ones are dropped; "wrap only what you use" stops working |
| `-Werror=switch` | top-level `CMakeLists.txt` | An appended enum value stops being a compile-time check (Rule 13) |
| Per-target `-Wl,--wrap=…` list | every consumer target | A call silently reaches the host |
| `-UNDEBUG` for tests | `tests/CMakeLists.txt` | Test asserts vanish — and note this does **not** protect the library's own header asserts ([DEF-3](roadmap.md#known-defects)) |

### `operator new` and the double link

Whether `operator new` reaches the wrappers depends on how `libstdc++` is linked. Under the
default shared link it does not: `operator new` lives in `libstdc++.so`, whose internal `malloc`
binding `--wrap` does not rewrite, so **`std::` containers are not interposed**. Under
`-static-libstdc++` they are.

This is why `tests/CMakeLists.txt` links the scenario suite a second way as
`test_scenario_static`, and why that target is *probed* rather than assumed — some toolchains
have no static `libstdc++`, and an unconditional target would break the build at link time rather
than skip.

**Do not rely on either behaviour.** A test that needs an exact eligible-call count must use the
raw allocator. This is why the worked example in [`fault-injection.md`](fault-injection.md) §17
allocates into a stack array.

---

## 5. Interposition boundaries

What `--wrap` structurally cannot reach. Full treatment:
[`IMPLEMENTATION_NOTES.md`](IMPLEMENTATION_NOTES.md).

| Boundary | Effect | Status |
|---|---|---|
| Same translation unit | Calls resolved without the wrap | Mitigated: wrappers live in their own TUs |
| glibc fortify substitution | `__read_chk` replaces `read` | Mitigated: `__wrap___read_chk` |
| `operator new` under shared `libstdc++` | `std::` containers not interposed | Pinned by the double link |
| Raw syscalls | Bypass everything | **Not mitigated** — user obligation 5 |
| Unwrapped POSIX families | `epoll`, `poll`, `select`, `sendmsg`, `recvmsg`, `getaddrinfo`, `mmap`, `sigaction`, `timerfd_*`, `pread`, `dup`, `fcntl`, rwlocks, barriers, semaphores | **Not mitigated** — [DEF-23](roadmap.md#known-defects) |

The last row matters more than it looks. A partially wrapped pthread family does not fail
loudly: the unwrapped call falls through to real glibc on an object the scheduler believes it
owns, which can block the single OS thread with no diagnostic
([DEF-17](roadmap.md#known-defects)).

---

## 6. Fault-model boundaries

Even where interposition works, some faults cannot be represented.

| Boundary | Status |
|---|---|
| `EINTR` and partial-sleep remainders | Not modelled — [DEF-24](roadmap.md#known-defects) |
| Short reads | Not modelled. `read` can only fire `ReadEio` |
| fd exhaustion (`EMFILE`/`ENFILE`) | Not modelled |
| Clock faults (`ClockStep`, `SleepInterrupted`) | Defined and validated, but the clock wrapper never consults the injector — [DEF-8](roadmap.md#known-defects) |
| Network faults | Defined and validated; the socket wrappers abort — [DEF-9](roadmap.md#known-defects) |
| Episodes (`crash_node`, `partition`, `pause_node`) | Config data only — [DEF-9](roadmap.md#known-defects) |

`validate()` currently accepts configuration in the last four rows. A rule there passes every
check and then never fires. This violates [I10](architecture.md#6-invariants) and is the highest
-value defect in the register, because it is silent.

---

## 7. Engine-side determinism invariants

The engine keeps its side of the contract through the invariants in
[`architecture.md`](architecture.md#6-invariants), in particular:

- **I8** — draws happen only at genuine choice points (`ready_.size() > 1`), so a single-fiber run
  consumes no schedule draws.
- **I5** — no allocation in wrapper paths, so a wrapped call cannot re-enter `__wrap_malloc`.
- **I3** — engine work is marked and passes through unfaulted.
- **I9** — determinism must not be a build-type property. **Currently violated.**

---

## 8. How to check a claim of non-reproducibility

In order:

1. Same binary, same build type, same seed, run twice. If these differ, it is the engine — file
   it against an invariant in [`architecture.md`](architecture.md#6-invariants).
2. Same binary, different build type. If only these differ, it is
   [DEF-3](roadmap.md#known-defects).
3. Same source, different compiler or optimisation level. If only these differ, it is
   [DEF-18](roadmap.md#known-defects).
4. Different binary or a changed wrapper set. Expected to differ — see the qualifiers in §1.

There is no automated way to do this yet. `--verify` double-run comparison is
[F5](roadmap.md#unified-roadmap).
