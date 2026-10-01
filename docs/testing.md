# Testing

How the suite is built, what each target proves, and the conventions a new test must follow. For
writing the thing being tested, see [`writing-a-wrapper.md`](writing-a-wrapper.md).

---

## 1. Running

```bash
just test              # or: nix develop -c just test
ctest --test-dir build --output-on-failure
```

All tests are hand-rolled `assert()`-based binaries registered with CTest. There is no test
framework dependency.

---

## 2. Build conventions

These are not preferences; each one exists because its absence broke something.

| Convention | Where | Why |
|---|---|---|
| `-UNDEBUG` forced on every test target | `tests/CMakeLists.txt` | Test asserts must not vanish in Release. **This does not protect library header asserts** — see [DEF-3](roadmap.md#known-defects) |
| `-fno-builtin-{malloc,free,calloc,realloc}` on targets that count allocations | `tests/CMakeLists.txt` | Otherwise the optimiser folds allocations before they reach the wrapper |
| Per-target `-Wl,--wrap=…` list | `tests/CMakeLists.txt` | Each test wraps only what it exercises. Subset wrapping is what makes this work ([I6](architecture.md#6-invariants)) |
| `-Werror=switch` | top-level `CMakeLists.txt` | Keeps Rule 13 (append-only enums) a compile-time property |

### The double link

`test_scenario_static` is a **second build of `test_scenario.cpp`**, linked with
`-static-libstdc++`. It exists because `operator new` reaches the wrappers only under static
`libstdc++`, and that is the only configuration in which the harness's own bookkeeping is visible
to the universe it reports on. `test_counters_are_snapshotted_at_quiesce` can only guard that
property in the static build.

It is **probed, not assumed**: `tests/CMakeLists.txt` compiles a throwaway program with
`-static-libstdc++` and skips the target when the toolchain has no static `libstdc++` — which
includes the repository's clang. An unconditional target would break the `build-clang` and
`build-asan` trees at link time rather than skip.

**Consequence:** in the clang tree, the `operator new` interposition property is unverified. Do
not read a passing `test_scenario` as evidence for it. See
[`determinism-contract.md`](determinism-contract.md) §4.

The same probe gates `test_broken_app_static`.

### White-box includes

`test_wrap_malloc.cpp`, `test_wrap_random.cpp`, `test_wrap_storage.cpp`, and
`test_scheduler.cpp` include `wrapper_fault.hpp` directly. That header lives in
`src/cosmos/wrappers/`, so these tests reach into `src/` for it. This is the current arrangement,
recorded in [`architecture.md`](architecture.md#9-header-layers). When adding a test that needs
the eligibility predicates or `ReentrancyGuard`, follow the existing include rather than
duplicating the predicate — a duplicated eligibility rule is a rule that will drift.

---

## 3. Scenario lifecycle

The harness enforces a strict order, and deviations are **recorded as failures, not asserted**:

```
create  ->  run (once)  ->  quiesce (idempotent)  ->  check
```

| Stage | Contract |
|---|---|
| `create` | Validates the `FaultConfig` and installs faults once. A rejected config returns a `ConfigProblem` rather than throwing |
| `run` | Exactly once. Calling it twice, or after `quiesce`, records a `cosmos.lifecycle` failure rather than running a second, muted workload |
| `quiesce` | Idempotent. Opens a **permanent quiet window**, drains the fiber scheduler, and snapshots counters and leak state under that guard |
| `check` | Must follow `quiesce`. A `check` before `quiesce` is a lifecycle failure |

Details worth knowing before writing a test:

- **Zero checks is a vacuous pass** and is reported as `VACUOUS`, not as success.
- **The ledger is rendered before the universe is destroyed**, because afterwards no caller could
  print it. `print_report` appends `ledger_dump` when present.
- **Leak checking is opt-in** via `check_no_leaks`. It is deliberately *not* automatic at universe
  end: a workload that legitimately holds state past quiesce would false-positive on a blanket
  check. (An earlier revision of [`design.md`](design.md) described an automatic check; the code's
  position is the correct one.)
- **Counter snapshots are taken at quiesce**, under the quiet guard, and reported outside it.
- **The sugar has different leak semantics.** `cosmos::run` destroys the universe when it returns,
  so any block the workload hands back outlives its heap and is released through the orphan path.
  Nothing can count those blocks afterwards — a universe-end leak check can never fire for the
  sugar.

---

## 4. What each target proves

| Target | Covers | Proves roadmap row |
|---|---|---|
| `test_random` | SplitMix64 / xoshiro256** known-answer vectors, bounds, reproducibility | [F0](roadmap.md#unified-roadmap) |
| `test_seed_derivation` | Seed hierarchy, domain isolation, decorrelation, class sub-streams | [F0](roadmap.md#unified-roadmap) |
| `test_faults` | `FaultConfig::validate()` — rates, tables, triggers, legality matrix, append-only site numbering | [F1](roadmap.md#unified-roadmap) |
| `test_fault_injector` | Gate chain semantics, quiet windows, budgets, triggers, stream isolation | [F1](roadmap.md#unified-roadmap) |
| `test_fault_draw` | Draw math: Bernoulli equivalence, rate calibration over 100k trials, weight distribution | [F1](roadmap.md#unified-roadmap) |
| `test_ledger` | Ledger fields, overflow counting, doc-format printing | [F1](roadmap.md#unified-roadmap) |
| `test_faultinjection_phase1_gate` | Zero RNG leakage over 1M decisions; same-seed stream-position probes | [F1](roadmap.md#unified-roadmap) |
| `test_wrap_malloc` | The full memory-wrapper contract: canaries, OOM, realloc, ownership routing, orphan hygiene | [F2](roadmap.md#unified-roadmap) |
| `test_scenario` | Harness lifecycle, vacuity, ledger-on-failure, counters at quiesce | [F2](roadmap.md#unified-roadmap) |
| `test_broken_app` / `test_fixed_app` | End-to-end: one seed fails a broken app, passes once fixed | [F2](roadmap.md#unified-roadmap) |
| `test_time` | `Time`/`Duration` discipline, saturating arithmetic, `VirtualClock` POSIX semantics, wrapped clock calls | [Phase 1](roadmap.md#unified-roadmap) |
| `test_scheduler` | Fiber scheduler through the pthread wrappers: spawn/join/mutex/cond, interleaving replay, deadlock reported | [Phase 1](roadmap.md#unified-roadmap) |
| `test_simulator` | Simulator lifecycle, current-slot semantics, fault install | [Phase 1](roadmap.md#unified-roadmap) |
| `test_wrap_storage` | Storage eligibility, outcome legality, honest `ShortWrite`, `__read_chk` | [F3](roadmap.md#unified-roadmap) |
| `test_wrap_random` | `getrandom` argument-validation order, stream mapping, seed no-ops | [F3](roadmap.md#unified-roadmap) |
| `test_wrap_net` | **The abort contract only** — proves seven socket wrappers `SIGABRT` under a universe | pins [DEF-9](roadmap.md#known-defects) behaviour |
| `test_scenario_static` | `operator new` interposition under static `libstdc++` | [I7](architecture.md#6-invariants) |
| `test_broken_app_static` | Same, for the broken-app fixture | [I7](architecture.md#6-invariants) |

### Coverage gaps

There is no test for any of the following, and no test is currently possible for some:

- **Clock fault injection** — the wrapper implements none ([DEF-8](roadmap.md#known-defects)).
- **Network behaviour** — there is no simulated network; `test_wrap_net` tests the aborts.
- **`EINTR`, short reads, fd exhaustion** — not modelled ([DEF-24](roadmap.md#known-defects)).
- **Trace replay / minimization** — [F5](roadmap.md#unified-roadmap). Tests named "replay" are
  same-seed re-execution, which is a weaker property.
- **Combined fault classes in one run.** `test_scenario` and `test_broken_app` exercise memory
  faults and clock reads. No test mixes storage, time, threads, and memory faults together.
- **The example binaries.** `kv_store_sim` and `replicated_kv_sim` are not run by any test. Both
  are currently `printf` stubs.
- **The `--wrap` mechanism itself.** There is no negative test proving that a missed `-Wl,--wrap`
  flag is detected. The `__read_chk` gap was found by reading, not by a test.
- **Fuzzing or property-based testing.** `test_fault_draw`'s statistical bounds are the only
  property-flavoured tests in the tree.

---

## 5. Conventions for a new test

1. **Assert-based, single file, registered with `add_test`.** Follow the existing
   `add_executable` + `target_link_libraries(cosmos)` + `add_test` pattern.
2. **Wrap only what you exercise.** Give the target its own `-Wl,--wrap=…` list.
3. **Add `-fno-builtin-*` if you count calls** of a function that has a compiler builtin.
4. **Never assert on a draw count you did not pin.** If the test cares how many draws a sequence
   consumes, probe the stream position explicitly — that is what
   `test_faultinjection_phase1_gate` does to detect misplacement at 2⁻⁶⁴.
5. **Same-seed comparisons, not cross-build-type ones.** See
   [`determinism-contract.md`](determinism-contract.md) §1 for why a Debug-vs-Release comparison
   will not hold.
6. **Use the raw allocator for exact eligible counts**, not `std::` containers — see
   [`determinism-contract.md`](determinism-contract.md) §4.
7. **Pin invariants, not implementation.** `test_wrap_net` is the model: it asserts the
   documented abort contract and nothing about a network that does not exist.
