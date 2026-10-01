# Cosmos Roadmap & Known Defects

This is the single source of truth for what is built, what is partly built, and what is
deliberately not being built. Every other document links here instead of restating a phase or
sprint marker.

- **[Unified roadmap](#unified-roadmap)** — every plan phase, fault-injection sprint, and code tag
  on one status table.
- **[Known defects](#known-defects)** — behaviour the shipped code has that contradicts an
  invariant or a documented promise. These are recorded, not fixed; this repository's current
  scope is documentation, so each row is an honest description of what the code does today.

**Linking convention.** Reference a row by linking its ID to its section, e.g.
`[F5](roadmap.md#unified-roadmap)`. Do not copy a status word (`done`, `partial`, …) into another
document — a status that lives in two places is a status that will disagree.

**Owner** is the owning module, not a person: `libcosmos/core` (engine), `libcosmos/fault`
(`faults.hpp` / `fault_injector.hpp` / ledger), `wrappers/*` (interposition layer), `harness`
(`scenario.hpp`, campaign), `build`, `docs`.

---

## Status vocabulary

Exactly four values. Nothing else may appear in the Status column.

| Status | Meaning |
|---|---|
| `done` | Shipped and its exit proof passes. |
| `partial` | Some exit criteria met, some not. The gap is spelled out in the row. |
| `not-started` | No implementation. Any header or data structure present is inert. |
| `wont-do` | Deliberately out of scope. Do not implement without first editing this row. |

---

## Unified roadmap

| ID | Source | Content | Status | Owner | Exit proof |
|---|---|---|---|---|---|
| Phase 0 | `plan.md` | Study notes & design specification | `done` | `docs` | Six documents present under `docs/` before this file |
| F0 | `fault-injection.md` | Seeded RNG: `xoshiro256**` + `splitmix64`, five domain streams, per-class sub-streams, universe-seed derivation from `(campaign_seed, index)` | `done` | `libcosmos/core` | `test_random`, `test_seed_derivation` |
| F1 | `fault-injection.md` | `FaultRule` / `decide` split; `validate()`; append-only `SiteId`; gate chain with eligible-counter semantics; quiet windows; budgets; deterministic occurrence triggers; v1 ledger | `done` | `libcosmos/fault` | `test_faults`, `test_fault_injector`, `test_fault_draw`, `test_ledger`, `test_faultinjection_phase1_gate` |
| F2 | `fault-injection.md` | First adapter plus oracle surface: `__wrap_malloc` → `OutOfMemory`; `Scenario` / `FaultPlan` harness with `quiesce()` and `check()`; `FaultProfile` deleted | `done` | `harness` + `wrappers/memory` | `test_wrap_malloc`, `test_scenario`, `test_broken_app`, `test_fixed_app` |
| Phase 1 | `plan.md` | Core runtime & linker wrapper engine: `libcosmos`, `-Wl,--wrap` for the core POSIX surface, virtual clock, fiber scheduler | `partial` | `libcosmos/core` | **Present:** `test_time`, `test_scheduler`, `test_simulator`. **Missing:** trace hash (Phase 1 exit criterion), `--verify` mode, sancov guidance — no implementation anywhere |
| F3 | `fault-injection.md` | Wrapper expansion one call at a time: `calloc`/`realloc`; `open`/`read`/`write`/`fsync`; `send`/`recv`/`connect`; clock reads | `partial` | `wrappers/*` | **Done:** `test_wrap_malloc` (calloc/realloc), `test_wrap_storage` (storage family). **Not started:** `send`/`recv`/`connect` abort — see `abort_unimplemented` at `wrap_net.cpp:49,56,63,70,77,84,91`. **Inert:** clock reads are wrapped but never consult the injector, see [DEF-8](roadmap.md#known-defects) |
| F4 | `fault-injection.md` | Campaign runner (many seeds, `never_hit` tracking); swarm sampler `FaultConfig::sample`; mutually-exclusive categorical draw for multi-outcome sites | `partial` | `harness` + `libcosmos/fault` | **Done:** categorical draw (`test_fault_draw`). **Not started:** campaign runner, `FaultConfig::sample`. `StreamDomain::Swarm` and `Knob`/`KnobId` are validated and then never consumed |
| F5 | `fault-injection.md` | Decision-trace recording; replay with suppression (Rule 12); 1-minimal minimization; canonical trace encoding + FNV-1a trace hash | `not-started` | `libcosmos/fault` | No trace, replay, or hash code exists. Tests named "replay" are same-seed re-execution only (`test_fault_injector`, `test_fault_draw`) |
| F6 | `fault-injection.md` | Distributed faults: event queue, node registry, episode lifecycle, persistent faults, scheduled episode triggers, fault-model limits with recorded refusals, `Liveness` mode | `not-started` | `libcosmos/fault` | `ScheduledEpisode`, `CrashNode`, `Partition`, `PauseNode`, `Knob` are config data validated by `test_faults` and then never executed |
| Phase 2 | `plan.md` | Simulated network & socket wrapping: latency, loss, reorder, partition | `not-started` | `wrappers/net` | `net.hpp` is empty. `wrap_net.cpp` aborts on all seven socket calls. `test_wrap_net` pins the abort contract, not a network |
| Phase 3 | `plan.md` | Workloads & campaign runner: `gen::*` combinators, multi-core campaign, repro CLI, double-run trace verification | `not-started` | `harness` | `gen.hpp`, `campaign.hpp`, `assert.hpp` are empty stubs. No CLI binary exists |
| Phase 4 | `plan.md` | Simulated storage: torn writes, crash durability, page-cache model | `partial` | `wrappers/storage` | **Done:** point faults on `open`/`read`/`write`/`fsync` (`test_wrap_storage`). **Not started:** page cache, torn writes, crash durability. Real kernel I/O throughout — a faulted write leaves the file untouched |
| Phase 5 | `plan.md` | Exploration v2/v3: sancov guidance, `Snapshot` branching, decision-log minimization, chrome-trace export | `not-started` | — | Depends on [F5](roadmap.md#unified-roadmap) |
| Phase 6 | `plan.md` | Production hardening: native production build verification, packaging, documentation | `not-started` | `build` | `kv_store_prod` and `replicated_kv_prod` build, but both are `printf` stubs |
| Phase 7 | `plan.md` | Hypervisor substrate: `KvmSubstrate : ISubstrate`, unmodified VM guests | `wont-do` | — | Out of scope. See [`future-substrate.md`](future-substrate.md) |
| P1 | code comments | Core-runtime tag used in `ledger.hpp:11-12`, `time.hpp:133`, `fault_injector.hpp:16` | `done` | `libcosmos/core` | Superseded by [Phase 1](roadmap.md#unified-roadmap); the engine it names shipped |
| P2 / P2-S3 | code comments | Harness tag used in `scenario.hpp:166`, `ledger_print.hpp:52`, `tests/CMakeLists.txt` | `done` | `harness` | Superseded by [F2](roadmap.md#unified-roadmap) |
| P5 | code comments | Trace tag used in `ledger.hpp:14` | `not-started` | `libcosmos/fault` | Same scope as [F5](roadmap.md#unified-roadmap) |
| P6 | code comments | Tag used in `fault_injector.hpp:16` for a clock-implementation swap | `wont-do` | — | No roadmap row ever defined this. The comment means "the `ClockLike` parameter stays; only `VirtualClock`'s body may change" |
| — | `fault-injection.md` | Automatic insertion of faults into application logic | `wont-do` | — | Deliberately not claimed by the design (`fault-injection.md` §8.3) |
| — | `fault-injection.md` | Independent-per-kind Bernoulli stacking (multiple outcomes per call) | `wont-do` | — | Deferred pending evidence. v1 is mutually-exclusive per site (`fault-injection.md` §6.5) |
| — | `fault-injection.md` | Tier 2+ trigger state machines and arbitrary predicates over live state | `wont-do` | — | Deferred until Tier 1 triggers are shown insufficient (`fault-injection.md` §10.1) |

### Not wired despite validating clean

`FaultConfig::validate()` accepts configuration the engine can never honour. A rule in any of
these places is accepted, passes all checks, and then silently never fires — the worst failure
mode for a chaos tool. Until [DEF-8](roadmap.md#known-defects) and
[DEF-9](roadmap.md#known-defects) are resolved, treat these as unsupported:

| Rule site | Why it cannot fire |
|---|---|
| `clock_gettime`, `nanosleep` | `wrap_time.cpp` never calls the injector ([DEF-8](roadmap.md#known-defects)) |
| `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv` | `wrap_net.cpp` aborts before any decision |
| `crash_node`, `partition`, `pause_node` | No episode engine ([F6](roadmap.md#unified-roadmap)) |
| Any `Knob` | Nothing reads `FaultConfig::knobs` |

---

## Known defects

Behaviour the shipped code has that contradicts an invariant or a documented promise. Each row
links to the invariant it violates where one exists. These are described accurately in the
documentation rather than papered over.

| ID | Defect | Where | Violates | Impact |
|---|---|---|---|---|
| DEF-1 | Double free of a simulated pointer passes the *payload* address to `__real_free`. The first free erases the registry entry, so the second classifies as `None` and forwards a pointer 32 bytes past the real allocation | `wrap_memory.cpp:96` | Registry ownership invariant (`memory.hpp:147-150`) | Host heap corruption. Should instead be a clean, reportable finding |
| DEF-2 | `TrackedHeap` statistics and `next_alloc_id_` are mutated with no lock while `__wrap_free` can route an owned pointer in from any thread. `ownership_of()` and `deallocate()` are also separate steps, so two concurrent frees can both pass the lookup | `memory.hpp:230,238-267`; `wrap_memory.cpp:86-89` | One universe per OS thread | Data race; possible double release |
| DEF-3 | Three asserts in headers erase under `NDEBUG`: `assert(lo <= hi)` in `Rng::range`, the `class_of(site) == cls` cross-check in `decide`, and `new_size > 0` in `reallocate` | `random.hpp:56`, `fault_injector.hpp:56`, `memory.hpp:293` | Determinism across build types | Tests force `-UNDEBUG`; consumers do not. Behaviour differs between Debug and Release |
| DEF-4 | Canary corruption makes `deallocate()` return `false` and deliberately leak the block, but `__wrap_free` discards the result | `wrap_memory.cpp` | Fault observability | Memory corruption never reaches the ledger or any finding sink |
| DEF-5 | A deadlock outside the quiesce drain calls `abort()`, and `pthread_exit` on the main fiber calls `exit()` | `task.cpp:168-177`, `task.cpp:275` | Harness robustness | A workload deadlock kills the harness instead of producing a `ScenarioReport`. Only drain-time deadlocks are recorded |
| DEF-6 | Completed tasks are never reaped. `all_tasks_` only grows; joined and detached tasks keep their 256 KiB stacks until `~Scheduler` | `task.cpp:210`, `task.cpp:253-259` | — | Unbounded memory in thread-spawning workloads |
| DEF-7 | Double-joining a `Done` task succeeds twice and returns stale `retval`; there is no `joined` flag | `task.cpp:220-223` | — | Divergence from pthread semantics |
| DEF-8 | `wrap_time.cpp` never consults the fault injector, so `ClockStep` and `SleepInterrupted` can never be produced | `wrap_time.cpp` | Config honesty | Clock rules validate clean and silently never fire |
| DEF-9 | `validate()` accepts rules for sites no wiring can honour — network sites, event sites, knobs | `faults.hpp` | Config honesty | Silent no-op configuration |
| DEF-10 | The `VirtualClock` sleep comment still describes an "interim contract … until the P1 fiber scheduler exists". The scheduler shipped and `wrap_time.cpp` already suspends | `time.hpp:133-138`, `time.hpp:207-208` | — | Stale comment contradicts the code beside it |
| DEF-11 | Two competing sleep APIs. `VirtualClock::nanosleep` / `clock_nanosleep` advance the clock in place, while the wrappers route to `Scheduler::sleep_ns` and suspend | `time.hpp:209-266` vs `wrap_time.cpp:31-44` | Single owner for time | Calling the clock API directly silently violates the scheduler's timer model |
| DEF-12 | `cosmos::run` (free function) and `Scenario::run` (member) are different entry points with the same name | `scenario.hpp:84`, `scenario.hpp:298` | API clarity | Easy to call the wrong one |
| DEF-13 | `virtual_clock.hpp` is a shim whose stated reason — injector tests including it — is stale | `virtual_clock.hpp:5-8` | — | Two headers claim ownership of `VirtualClock` |
| DEF-14 | The reentrancy guard is applied inconsistently. Present in `wrap_memory`, `wrap_storage`, `wrap_threads`, and `getrandom`; absent from all of `wrap_time`, all of `wrap_net`, and four of five `wrap_random` functions | `wrapper_fault.hpp:15-27` | Reentrancy invariant | An engine-internal call on the unguarded paths would consume user RNG draws or inject a fault into itself |
| DEF-15 | Mutex and condition-variable identity is the raw pointer address | `task.hpp:56-63` | — | A stack object at a recycled address inherits stale lock state if `pthread_mutex_destroy` was never called |
| DEF-16 | `pthread_mutexattr_t` is ignored, so recursive, error-check, and robust mutexes silently become default | `task.cpp:282-284` | pthread fidelity | Recursive lock returns `EDEADLK` |
| DEF-17 | `pthread_cond_timedwait`, `pthread_mutex_timedlock`, rwlocks, barriers, semaphores, `pthread_once`, TLS keys, and `pthread_attr` are not wrapped | `wrap_threads.cpp` | pthread fidelity | These fall through to real glibc on objects the scheduler believes it owns, which can block the single OS thread |
| DEF-18 | Fault outcomes are chosen by accumulating and dividing doubles | `fault_injector.hpp:27-31`, `faults.hpp:644-659` | Cross-compiler replay | FMA and `-ffp-contract` differences can move a bucket boundary. Same-compiler replay is reliable; cross-compiler is not |
| DEF-19 | The ledger is a fixed 256-entry buffer. Overflow is counted in `dropped_` and the tail is lost | `ledger.hpp:30-41` | Replay inputs | Long runs lose their decision history silently |
| DEF-20 | `Simulator::set_current` is public with no ownership or thread check | `simulator.hpp:48` | One universe per OS thread | Any code can hijack the thread's active universe |
| DEF-21 | `Task::wake_error` is assigned zero in every path and its consumers are unreachable. `Timer::seq` and `timer_seq_` are assigned and never read | `task.hpp:47-48`, `task.hpp:72` | — | Dead scaffolding for a deadlock-fallback wake contract that was never implemented |
| DEF-22 | `Scenario::run` propagates an exception from the workload without recording a lifecycle entry | `scenario.hpp:84` | Lifecycle enforcement | The exception escapes the harness rather than becoming a report |
| DEF-23 | No I/O readiness, message-socket, name-resolution, timer, or signal family is wrapped. `pread`, `pwrite`, `fdatasync`, `ftruncate`, `rename`, `unlink`, `mkdir`, `dup`, `fcntl`, `mmap`, `sigaction`, `epoll`/`poll`/`select`, `sendmsg`/`recvmsg`, `getaddrinfo`, `timerfd_*` are all absent | `src/cosmos/wrappers/` | POSIX coverage | An application using any of these runs partly un-simulated with no diagnostic |
| DEF-24 | `EINTR`, short reads, and fd exhaustion (`EMFILE`/`ENFILE`) are not modelled. `read` can only fire `ReadEio`; `open` only `OpenEio` / `NoSpace` | `wrap_storage.cpp`, `faults.hpp:154-174` | Fault-model breadth | Standard DST surfaces are unrepresentable |
