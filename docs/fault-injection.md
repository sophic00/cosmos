# Cosmos: Fault Injection Design

This document defines how `libcosmos` breaks things on purpose.

Fault injection is the part of the simulator that makes rare, unlucky situations happen often: memory running out, packets vanishing, disks corrupting, servers dying at the worst possible moment. Its job is **not** to decide whether the application is correct — that is the assertion layer's job (`always` / `sometimes`, see `docs/design.md` §12). Its job is to steer execution into the error paths that normal testing never reaches.

> **Core rule:** a fault is an **input**, not a failure. `malloc` returning `nullptr` is not a bug — it is a normal thing that happens on real machines. The bug is whatever the application does next.

Research background: **`docs/antithesis-study-notes.md`**. Full API reference: **`docs/design.md`**. Architecture & seams: **`docs/architecture.md`**.

---

## Table of Contents

1. [Concepts](#1-concepts)
2. [The Fault Model (What the System Promises to Survive)](#2-the-fault-model-what-the-system-promises-to-survive)
3. [Safety Mode vs Liveness Mode](#3-safety-mode-vs-liveness-mode)
4. [The Three Shapes of a Fault](#4-the-three-shapes-of-a-fault)
5. [Block-Level Architecture](#5-block-level-architecture)
6. [Two-Level Probability & Swarm Configuration](#6-two-level-probability--swarm-configuration)
7. [RNG Stream Discipline](#7-rng-stream-discipline)
8. [Injection Sites: Reaching the Operating System Boundary](#8-injection-sites-reaching-the-operating-system-boundary)
9. [Run Lifecycle & Quiet Windows](#9-run-lifecycle--quiet-windows)
10. [The Decision Gate Chain](#10-the-decision-gate-chain)
11. [The Fault Ledger, Decision Trace & Minimization](#11-the-fault-ledger-decision-trace--minimization)
12. [Public API Reference](#12-public-api-reference)
13. [Determinism Rules (Summary)](#13-determinism-rules-summary)
14. [Testing the Injector Itself](#14-testing-the-injector-itself)
15. [Implementation Roadmap](#15-implementation-roadmap)
16. [References](#16-references)
17. [Worked Example: Specifying, Injecting, and Diagnosing](#17-worked-example-specifying-injecting-and-diagnosing)

---

## 1. Concepts

| Term | Meaning |
|---|---|
| **Fault** | A bad-but-legal thing the simulator makes happen (memory failure, dropped packet, crashed node). |
| **Fault Model** | The written promise of which failures the application claims to survive. Faults outside it produce false bug reports. |
| **Fault Class** | A family of faults owned by one subsystem: `Memory`, `Network`, `Storage`, `Clock`, `Process`, `Random`. |
| **Point Fault** | An instant, one-off fault (`malloc` fails). Decided by one dice roll at the call. |
| **Episode Fault** | A fault with a duration that must later heal (a network partition from t=30ms to t=45ms). |
| **Knob Fault** | Nothing breaks; a normal tuning value is set to an extreme-but-legal value (a 60s timeout becomes 0.1s). |
| **Swarm** | Choosing a *different* subset of fault classes and intensities for every universe, instead of one uniform setting for all. |
| **Injection Site** | A specific place a fault can be decided: one particular `__wrap_*` function (a **wrapper site**, e.g. `malloc`, `send`) or one named engine event (an **event site**, e.g. a scheduled node crash — §10.2). |
| **SiteId** | A stable, compile-time-assigned numeric ID per injection site (§6.2). Append-only: once assigned, an ID is never renumbered or reused. |
| **Fault Ledger** | The timestamped **diagnostic** record of a universe: injected faults, limit refusals, heals, and per-site counters. Answers "what happened?" — used for failure reports and coverage (§11.1). |
| **Decision Trace** | The ordered record of every RNG-affecting fault decision, recorded for replay. Answers "how do I re-run it?" — the input to suppression and minimization (§11.2, sprint F5). A separate artifact from the diagnostic ledger. |
| **Quiet Window** | A span of the run during which no faults may fire (engine setup, app warmup, final settle-down). |

---

## 2. The Fault Model (What the System Promises to Survive)

Every system under test carries a promise. For example:

> *"With 3 nodes, I keep all committed data even if any 1 node dies at any moment."*

That promise is the **fault model**. It is the boundary line for the injector.

Compare a building rated for a magnitude-7 earthquake. Hit it with a 9 and it falls down — that is not a construction defect. The same logic applies here:

| What the injector does | Result | Verdict |
|---|---|---|
| Kills 1 of 3 nodes | Data survives | Correct behaviour |
| Kills 1 of 3 nodes | Data lost | **Real bug** |
| Kills all 3 nodes | Data lost | **Not a bug** — the promise was exceeded |

**Why this must live in the config, not in the user's head:** an injector that does not know the fault model will happily kill all 3 nodes and then report "DATA LOSS" on every run. Every one of those findings is a false positive, and they drown the real ones.

So `FaultConfig` carries **limits**, not just rates:

```cpp
uint32_t max_crashed_nodes  = 0;   // never crash more than this many at once
uint32_t min_healthy_quorum = 0;   // always leave at least this many reachable
```

Before an episode fault starts, the injector checks the limits. If starting it would break the promise, the fault is **not** started — and that decision is written to the ledger as a `Skipped` record with its reason (§11.1). A skip is recorded explicitly rather than left as a gap, because replay and minimization must never have to infer what happened from a *missing* entry.

---

## 3. Safety Mode vs Liveness Mode

There are two very different kinds of promise, and **they cannot be tested under the same conditions**.

**Safety — "I never lose or corrupt your data."**
This must hold no matter how bad things get. A completely frozen database that serves nobody has still not *lost* anything. So safety is tested under **maximum chaos**.

**Liveness — "I actually respond and make progress."**
If every node is dead and every cable is cut, of course nobody gets served. That is physics, not a bug. To test liveness you must first make the world *reasonably healthy*, then demand progress.

TigerBeetle's VOPR does exactly this. In liveness mode it picks a quorum of replicas to be the "core", **restarts any core replicas that are down, heals all partitions between core replicas**, and makes every fault involving non-core replicas permanent. Then it demands progress within a deadline.

| Mode | World condition | Assertions checked |
|---|---|---|
| `Safety` | Unbounded chaos, within the fault model | `always` invariants: nothing lost, nothing corrupted |
| `Liveness` | A healthy quorum is force-healed | Progress deadlines: work actually completes |

**Note on "permanent":** entering `Liveness` mode is a **one-way mode transition**, not ordinary fault activity. At the transition the injector force-heals every fault touching a core node, and *converts* every still-active fault on a non-core node into a **persistent fault** (§4.2.1) whose scheduled heal is cancelled. This is a deliberate, mode-specific exception to the normal episode lifecycle — it is the only place where an already-scheduled heal may be withdrawn. The quiesce phase (§9.3) still force-heals everything, persistent faults included; otherwise the final validation phase could never observe a recovered system.

**The trap this avoids:** writing `always(commit_completes_within(5s))`, running it under full chaos, and watching it fail on every seed for entirely legitimate reasons. The application looks broken when the *test setup* is what is wrong.

---

## 4. The Three Shapes of a Fault

Each shape plugs into a different part of the engine, so this classification is structural, not cosmetic.

### 4.1 Point Faults — the pothole

Instant, one-off, over immediately.

- `malloc` returns `nullptr`, `errno = ENOMEM`
- `write` returns `EIO`
- One packet is dropped

**Machinery:** a hook in the `__wrap_*` function. One dice roll, immediate answer, no follow-up.

### 4.2 Episode Faults — the road closure

These have a **start**, a **duration**, and a mandatory **end**.

- Network split from t=30ms to t=45ms
- A node paused for 500ms
- A node crashed, rebooting after 2s

**Machinery:** the virtual event queue. Starting an episode **must** also schedule its own heal event at `now + duration`.

> An episode fault that never heals is a bug in the injector, not the application. Without healing you can never test *recovery*, which is usually the most interesting behaviour. Antithesis models this explicitly with a `Restore` fault that clears all active network faults.

#### 4.2.1 Persistent faults — the road that stays closed

A small number of faults have **no natural end**: a clock stepped permanently forward, a disk sector corrupted for the rest of the run, or a non-core node abandoned after the `Liveness` transition (§3).

These are **not** episode faults, and the doc keeps them separate precisely so the "every episode schedules its own heal" rule stays absolute. A persistent fault:

- is recorded in the ledger with `duration = ∞` and no scheduled heal event,
- is **only** created by an explicit persistent-fault request or the `Liveness` mode transition — never by an episode "forgetting" to heal,
- is still cleared unconditionally by the quiesce phase (§9.3).

| Shape | Has a heal event? | Cleared at quiesce? |
|---|---|---|
| Episode | Yes, scheduled at start | Yes (early, if still active) |
| Persistent | No, by design | Yes |

The distinction matters because "no heal event was scheduled" is a **bug** for an episode and **correct** for a persistent fault. Without two named shapes, the injector cannot tell those two situations apart.

### 4.3 Knob Faults — the absurd speed limit

Nothing breaks. A normal tuning value is simply set to an extreme but perfectly legal value.

FoundationDB's example: a production timeout of **60 seconds** becomes **0.1 seconds** in simulation — 600× shorter. Nothing is broken; that is a valid setting. But the "peer did not reply in time" code path, which normally runs almost never, now runs constantly. That path is usually where the bugs are hiding, precisely because nobody exercises it.

FDB randomises **hundreds** of such knobs per run: timeouts, cache sizes, I/O block sizes, buffer lengths.

**Machinery:** sampled once per universe from the `Swarm` stream, before the run starts. No runtime hook at all.

**Delivery boundary (important):** a knob is an *application* tuning value, and Cosmos tests the application as a black box — Cosmos cannot reach in and turn the knob itself. Delivery is **harness-mediated**: the test harness reads the sampled knob values out of the universe's `FaultConfig` and passes them to the application the way an operator would — environment variables, CLI flags, or a config file — *before the application starts*. Cosmos owns sampling and per-universe storage; the harness owns delivery. A knob value is **static for the whole universe**: sampled once, never mutated mid-run (a value that changes mid-run is an episode fault, not a knob — Rule 14).

This is the cheapest high-yield technique in the whole design: it costs almost nothing to implement and it makes rare code paths common.

---

## 5. Block-Level Architecture

```mermaid
graph TD
    Seed["64-bit Universe Seed"]

    Seed --> SwarmRng["Swarm RNG<br><i>drawn once, before the run</i>"]
    Seed --> FaultRng["Fault RNG Stream<br><i>domain = 2</i>"]

    SwarmRng --> Config["<b>FaultConfig</b><br>• which fault classes are ON<br>• rate sampled per class<br>• knob values<br>• fault-model limits<br>• mode: Safety or Liveness"]

    FaultRng --> MemS["Memory sub-stream"]
    FaultRng --> NetS["Network sub-stream"]
    FaultRng --> StoreS["Storage sub-stream"]
    FaultRng --> ClockS["Clock sub-stream"]

    Config --> Injector["<b>FaultInjector</b><br>gate chain + budgets + quiet windows"]
    MemS --> Injector
    NetS --> Injector
    StoreS --> Injector
    ClockS --> Injector

    Injector --> Wrappers["__wrap_* POSIX wrappers<br><i>malloc, send, write, clock_gettime</i>"]

    Wrappers --> Ledger["<b>FaultLedger</b><br>fires, refusals, counters<br><i>(+ decision trace from F5)</i>"]

    Ledger --> Report["Failure report + repro seed"]
    Ledger --> Minimize["Minimization<br><i>replay with faults suppressed</i>"]
    Ledger --> Hash["Trace hash for --verify"]
```

Read it top to bottom: **the seed decides the shape of the universe, the config decides what is allowed to break, the injector decides whether a specific fault fires right now, and the ledger remembers what happened — so the run can be explained, and (from sprint F5, via the decision trace) replayed and minimized.**

---

## 6. Two-Level Probability & Swarm Configuration

This is the single most important design decision in the fault engine.

### 6.1 The weak approach

The obvious design is one fixed rate per fault type, applied uniformly to every run: *"every allocation has a 1% chance of failing, every packet a 1% chance of dropping."*

This is much weaker than it appears. **Every run looks statistically identical** — a light, even drizzle of chaos everywhere, with nothing ever hit hard. Ten thousand runs produce ten thousand nearly identical mediocre tests.

### 6.2 The two-level approach

FoundationDB's `BUGGIFY` splits the decision in two:

| Level | Decided | Default | Question answered |
|---|---|---|---|
| **Activated** | Once per run, per site | 25% | Is this fault site enabled *at all* in this universe? |
| **Fired** | Every time the site is reached | 25% | Given it is enabled, does it fire *this time*? |

The effect: run A has sites `{3, 17, 42}` enabled and hammers them relentlessly; run B has `{5, 9}` enabled and hammers those. Every run is a **different shape of storm** rather than the same grey drizzle.

**This requires site identity, not just class identity.** A fault *class* (`Memory`) is far too coarse to express "sites 3, 17 and 42 are active" — enabling the class would activate every memory site at once, collapsing the two levels back into one. So Cosmos gives every injection site a **stable `SiteId`**:

- **Wrapper sites** (§8): a compile-time-assigned ID per wrapped function (`site::malloc`, `site::send`, …).
- **Event sites** (§10.2): a compile-time-assigned ID per named engine event that can trigger an episode fault (`site::crash_node`, `site::partition`, …). Event sites exist because process/topology faults have no POSIX call to wrap — there is no `crash()` symbol in the application's binary — so they are triggered by the harness or the virtual-time schedule rather than by interposition.

Both kinds are **named, not positional**, so they stay stable across source edits. The `SiteId` enumeration is **append-only**: a new site always gets a fresh ID, and a retired site's ID is never reused. Activation sets, ledger entries, decision traces, and repro commands all key on `SiteId`; renumbering would silently re-point every historical repro at the wrong site (Rule 13, §13).

Because a reproduction is identified by **`(build, seed)`**, not by a seed alone — any source change alters the compiled code and therefore the execution, so a seed was never portable across builds in the first place, and TigerBeetle makes this explicit by pairing every reproduction with a **git commit hash** — a compile-time-assigned wrapper ID is all activation and replay ever need.

The gate then has three levels, coarse to fine:

```text
class enabled?   →  site activated?  →  fires this time?
(swarm, per run)    (swarm, per run)    (per event)
```

The same `SiteId` is what the ledger stores for replay identity (§11.2), so site identity is needed twice over and is worth defining once, properly.

### 6.3 Why it works — swarm testing

This is **swarm testing** (Groce et al., ISSTA 2012). Their experiment ran a random tester two ways: one heavily hand-tuned configuration, versus a swarm of random configurations where **each one deliberately omits some features**. The swarm found **42% more distinct ways to crash a collection of C compilers.**

Two reasons, both of which apply directly here:

1. **Some activities actively hide bugs.** Their example: frequent `pop` calls stop a stack from ever reaching its overflow-detection bug. Applied here — if a run keeps freeing memory, it may never reach the exhaustion path.
2. **Everything competes for room.** A run doing memory *and* network *and* crash faults splits its attention three ways. A run doing *only* memory faults explores memory-failure territory far deeper.

> **A run that injects fewer kinds of faults is not a weaker run.** It is a differently-shaped one, and a good campaign needs both kinds.

### 6.4 How Cosmos applies it

Three layers, all derived from the seed:

```text
Level 0  (per campaign)  campaign_seed + universe_index  →  universe_seed
Level 1  (per universe)  which fault classes are enabled       ← swarm
                         which sites are activated             ← swarm
                         each site's rule rate                 ← sampled, not hardcoded
                         knob values                           ← sampled
Level 2  (per event)     one draw selects pass-through or one FaultKind
```

#### Universe seed derivation

A campaign must not simply hand out `campaign_seed + 1`, `+ 2`, … — adjacent seeds produce correlated streams in most PRNGs. Each universe seed is derived by mixing:

```text
universe_seed = splitmix64(campaign_seed ^ splitmix64(universe_index))
```

This makes universe *N* reproducible from `(campaign_seed, N)` for a **given build**, which is what the `--seed` repro command depends on. Seeds are not portable across builds — any source change alters the execution — so a reproduction is properly identified by `(build, seed)`, exactly as TigerBeetle pairs a seed with a git commit hash.

#### The swarm stream needs its own domain

Config sampling must not consume from the `fault` stream, or the config itself would shift every runtime fault draw. `Swarm` is therefore a **fifth stream domain** alongside the four in `docs/design.md` §7 — see Rule 1 in §7 below.

#### Sample every class, including disabled ones

This is the subtle part, and it is the **opposite** of Rule 3 (§7).

When sampling the config, draw the rate for **every** fault class at a **fixed, class-indexed position** — even for classes this universe has disabled. The disabled classes' values are simply discarded.

**Why:** if you sampled only the enabled classes, then flipping `Network` from off to on would consume an extra draw and shift the rate that `Storage` and every later class receives. Two universes that differ only in whether the network is enabled would end up with unrelated storage rates, and the swarm dimensions would no longer be independent.

The cleanest implementation is to give each class its own sampling sub-stream, so class *k*'s rate depends only on the universe seed and *k*.

**Concretely:** the sampler derives one sub-stream per fault class from the `Swarm` stream (`derive_seed(swarm_seed, class_index)`), and draws that class's enable bit, site-activation set, and rule rates from it in a fixed, class-indexed order. Knob values are drawn from their own per-knob positions in the same `Swarm` stream, so adding a new knob never shifts any fault class's draws (and vice versa).

> **Rule 3 and this rule are not in conflict — they govern different phases.** Config sampling happens **once, before the run**, where a fixed draw schedule keeps the dimensions independent. Runtime fault decisions happen **per event**, where skipping draws is what keeps a disabled fault from disturbing the others. Fixed schedule when sampling; skip-when-impossible when firing.

**Do not hardcode a rule's rate.** Writing `rules[site::malloc].rate = 0.001` and leaving it explores exactly one point in the space. Let the seed *sample* a rate per universe — log-uniform between `1e-5` and `1e-2`, say. The same applies to storage, network, clock, and process outcomes: rare and frequent versions of a legal API failure expose different behavior, while a fixed constant explores only one point.

### 6.5 Multiple Fault Kinds at One Site — Mutually Exclusive Selection (v1)

Some sites can fail in more than one way. `send()` can drop the packet, delay it, reorder it, or corrupt it — one site, several possible outcomes. §6.2 only decided *whether* a site fires; it did not say what happens when a firing site has a menu of outcomes to choose from.

> **There is no global fault chooser.** The engine never asks "which fault should happen now?" from a global menu of all fault types. The application's own call determines the menu: a `malloc()` call can only ever produce a memory outcome, a `send()` call only a network outcome. The draw below merely selects between *pass-through* and *one outcome from this specific site's table*. The only cross-cutting choices are made once per universe, by the swarm sampler (§6.4) — never at runtime.

**v1 rule: at most one outcome per call, chosen categorically.** The site's final decision is not "roll a separate Bernoulli trial per outcome and possibly stack several." One draw first decides whether the site's configured rate fires; if it does, that same draw is rescaled and used for a small categorical outcome table:

```cpp
struct SiteOutcome {
    FaultKind kind;    // e.g. PacketDrop, PacketDelay, PacketCorrupt
    double    weight;  // relative weight; any positive scale, normalized by normalize()
};
using OutcomeTable = std::vector<SiteOutcome>;   // one table per multi-outcome site
```

The draw is a single number from the site's own sub-stream, walked against the cumulative weights until it lands:

```text
draw = class_stream.uniform()   // one draw, same as any other site
if draw >= rule.rate: return None
cumulative = 0
for outcome in table:
    cumulative += outcome.weight
    if draw / rule.rate < cumulative: return outcome.kind  // exactly one outcome
```

This is still exactly one RNG draw — it just returns a category instead of a boolean, so Rule 2/Rule 3 (§7) hold unchanged.

**Why mutually exclusive, and why now:** a single call producing one clean, nameable outcome keeps the ledger easy to reason about (`site=send → outcome=PacketDrop`, never "drop *and* corrupt, in some order") and keeps minimization simple — suppressing one ledger line always means "this call behaved normally," never "this call partially misbehaved." Independent-per-kind Bernoulli trials (each kind rolling its own coin, so several can stack on the same call) is a real technique too — it models compounding real-world failures more faithfully — but it is deliberately **deferred to a later version**. Ship the simpler mutually-exclusive model first, validate it against real bugs, then upgrade individual fault classes to independent stacked trials once there's evidence the extra compounding is worth the added complexity in the ledger and in minimization.

| Model | Outcomes per call | Ledger entry | Status |
|---|---|---|---|
| Mutually exclusive (categorical draw) | 0 or 1 | Always a single named outcome | **v1, current** |
| Independent Bernoulli per kind (stacking) | 0, 1, or more | Multiple outcomes on one call | Future upgrade, per fault class |

Single-outcome sites (`malloc` only ever has one failure mode: OOM) are the degenerate case of the same mechanism: a one-entry outcome table. The injector has **one code path** — draw once, compare against the rate, walk the cumulative weights — and a one-entry table simply makes the walk return immediately. There is no separate "plain Bernoulli" branch to maintain or test differently. A rule with a nonzero rate or a trigger must name at least one outcome; `validate()` rejects a rule that can fire but has nothing to fire with (§12).

---

## 7. RNG Stream Discipline

Fault injection is where determinism most easily springs a leak. Four rules.

### Rule 1 — Faults draw only from the `fault` stream

Never share with `schedule`. Stream domains extend the four fixed in `docs/design.md` §7 with a fifth for config sampling:

| Domain | Value | Used for |
|---|---|---|
| `Schedule` | 1 | Task interleaving choices |
| `Fault` | 2 | Runtime fault decisions (per event) |
| `Workload` | 3 | Generated test data |
| `User` | 4 | Application-visible `getrandom` / `random` |
| `Swarm` | 5 | Per-universe config sampling (§6.4) |

`Swarm` is separate from `Fault` so that sampling the configuration cannot disturb the runtime draws that configuration then governs.

### Rule 2 — Every fault class gets its own sub-stream

Derive `fault` into one sub-stream per class: `Memory`, `Network`, `Storage`, `Clock`, `Process`, `Random`.

`Random` covers `getrandom`, whose *values* come from the `User` stream (domain 4) so the application sees a reproducible sequence. Whether the call *fails* is a separate decision and belongs to the `Fault` stream like any other site, so the two never share draws.

**Why:** with a single shared stream, enabling network faults adds extra draws, which shifts every later draw, which means memory faults suddenly happen in completely different places. You could never change one setting at a time.

#### What this guarantee does and does not cover

The guarantee is about the **RNG stream**, not about the resulting ledger:

> Changing the `Network` configuration leaves the **sequence of values drawn from the `Memory` sub-stream** unchanged. The *k*-th memory draw returns the same number it would have before.

It does **not** promise a byte-identical memory ledger, and claiming otherwise would be wrong. Network faults legitimately change control flow: the application retries differently, allocates a different number of times, and reaches memory sites at different virtual timestamps. So the memory ledger's *timestamps*, *call counts*, and *which* allocation was the unlucky one can all legitimately move.

The practical benefit still holds, and it is the one that matters when debugging: **the memory sub-stream is not perturbed**, so a memory fault that fires on the *k*-th eligible allocation still fires on the *k*-th eligible allocation. You are varying one dimension while the other's decision function stays fixed. See the corresponding test in §14, which compares decisions rather than ledger bytes.

### Rule 3 — Never draw for a fault that cannot fire

All cheap gates (disabled, quiet, warmup, budget exhausted) must return **before** touching the RNG. An unused draw still consumes a number and shifts everything after it — meaning turning a fault *off* would change unrelated results, defeating the whole point.

### Rule 4 — Never draw inside an order-dependent loop

Iterating a container in raw pointer/address order and drawing per element reintroduces ASLR non-determinism through the back door. See the determinism contract in `docs/plan.md` §8.

---

## 8. Injection Sites: Reaching the Operating System Boundary

Cosmos keeps application source untouched. A simulation build links a selected
set of operating-system boundary calls through `__wrap_*` functions, and each
wrapper asks the same `FaultInjector` for a decision. The first adapters are
memory allocation; storage, network, and clock adapters reuse the same decision
engine as they are added. (Episode faults — crashes, partitions — have no POSIX
call to wrap; they use event sites instead, see §10.2.)

### 8.1 What linker wrapping can reach

`-Wl,--wrap=<symbol>` rewrites calls that resolve through `<symbol>` in the
final test link. For example, `--wrap=malloc` routes such calls to
`__wrap_malloc`, which may return `nullptr`/`ENOMEM` or delegate to
`__real_malloc`. No application source file is edited.

The scope is precise: wrapping reaches only calls that resolve through symbols
Cosmos explicitly wraps. It does not promise to intercept direct syscalls,
custom allocators, all C++ allocation implementations, or separately loaded
code that bypasses those symbols.

### 8.2 The generic adapter contract

Every wrapper follows the same three steps:

```text
1. Name its FaultClass and stable SiteId.
2. Ask FaultInjector to decide the one permitted fault outcome for this call.
3. Either translate that outcome into the real API's documented failure result,
   or call the original function unchanged.
```

For example, the memory adapter translates `OutOfMemory` to `nullptr` plus
`errno = ENOMEM`; a storage adapter can translate `WriteEio` to `-1` plus
`errno = EIO`; a network adapter can translate `ConnectionReset` to `-1` plus
`errno = ECONNRESET`. Cosmos never invents an application-specific return
value—it only produces results already valid for the wrapped API.

| Adapter | Class / site | Example injected outcome | What the caller sees |
|---|---|---|---|
| `__wrap_malloc` | `Memory` / `site::malloc` | `OutOfMemory` | `nullptr`, `errno = ENOMEM` |
| `__wrap_write` | `Storage` / `site::write` | `WriteEio` | `-1`, `errno = EIO` |
| `__wrap_send` | `Network` / `site::send` | `ConnectionReset` | `-1`, `errno = ECONNRESET` |
| `__wrap_clock_gettime` | `Clock` / `site::clock_gettime` | `ClockStep` | A configured, valid clock result |

**Legality includes API invariants, not just return codes.** An injected result must be one the real API could actually produce under *some* real circumstance. `EIO` from `write` is legal. A short write is legal. A forward leap in `CLOCK_MONOTONIC` is legal (suspend/resume produces exactly that). But `CLOCK_MONOTONIC` jumping *backward* is **not** legal — no real kernel does that — so a `ClockStep` outcome on a monotonic clock may only step forward. Similarly a short `send` must report a non-negative byte count, and `open` may not fail with `ECONNRESET`. A fault that violates its API's invariants is not testing the application's error handling; it is testing its reaction to an impossible world, and any resulting finding is a false positive (Rule 15).

### 8.3 What this guarantees

- **The application's source is never touched.** Simulation changes linking for selected test targets only.
- **Each injected result is legal for its wrapped API.** The application observes an ordinary operating-system failure and follows its own existing error path.
- **The decision mechanism is generic.** Classes, sites, rates, budgets, deterministic triggers, ledger entries, and replay all use one shared model; wrappers only perform API-specific translation.
- **The reach is intentionally bounded.** Cosmos tests the error handling exposed at supported operating-system boundaries; it does not guess whether an arbitrary internal application branch is safe to force.

| Mechanism | App changes | Reach | Who uses it |
|---|---|---|---|
| `__wrap_*` adapters | None | Calls resolving through explicitly wrapped OS/API symbols | Every supported simulation build |

---

## 9. Run Lifecycle & Quiet Windows

A universe has four phases. Faults are only allowed in one of them.

```mermaid
graph LR
    A["<b>1. Warmup</b><br>app starts up<br><i>faults OFF</i>"]
    B["<b>2. Chaos</b><br>workload runs<br><i>faults ON</i>"]
    C["<b>3. Quiesce</b><br>heal everything,<br>let it settle<br><i>faults OFF</i>"]
    D["<b>4. Validate</b><br>final invariants<br>checked"]

    A --> B --> C --> D
```

Three distinct reasons to suppress faults:

### 9.1 Engine-internal quiet — do not sabotage the referee

When `libcosmos` allocates for its own bookkeeping, that must **never** fail. A memory fault inside the simulator's own machinery corrupts the simulation itself and makes every result meaningless. Two mechanisms share this duty: the **reentrancy guard** already present in `wrap_memory.cpp` (a `thread_local` flag that makes any allocation entered *from within a wrapper* bypass the injector entirely — this protects the allocator's own metadata operations), and the **quiet window** (`push_quiet`/`pop_quiet`, usable by any engine code, and by the harness for app-requested critical sections). The guard is the automatic, zero-ceremony case; quiet windows are the explicit, general case. Both are checked before any draw, so neither perturbs the RNG streams (Rule 3).

### 9.2 Warmup window — do not trip the runner before the race

Do not inject during application startup. *Every* program dies if memory fails while it is loading its initial state, so you learn nothing and get thousands of identical uninteresting failures. Let the application boot cleanly, then start the chaos.

This is the Linux kernel fault-injection framework's `space` parameter (skip the first N calls). Antithesis exposes an application-driven version so code can request quiet around genuinely critical sections.

### 9.3 Quiesce window — let the dust settle before the photo

At the end of a run: **stop all faults, heal every partition, restart downed nodes, drain the event queue, and only then check the final invariants.**

**Why this is essential:** many of the most valuable checks are impossible without it. Take *"all three replicas agree on the data."* That cannot be checked while the network is split — of course they disagree; they cannot talk. Disagreement there is *correct*. The only meaningful version of the check is *"after the network heals and things settle, do all three agree?"*

Without a quiesce phase, every eventual-consistency invariant is simply unassertable. Antithesis provides this as a terminal pause via `eventually` / `finally`.

---

## 10. The Decision Gate Chain

Every point-fault decision runs the same ordered chain. All cheap gates first; **exactly one dice roll, at the end.**

```mermaid
flowchart TD
    Start(["decide(class, site)"]) --> G1{"Quiet window?<br><i>engine work, or quiescing</i>"}
    G1 -- yes --> Skip["<b>Skipped</b><br>drew = no"]
    G1 -- no --> G2{"Is the fault class<br>enabled this run?"}
    G2 -- no --> Skip
    G2 -- yes --> G2b{"Is THIS SITE<br>activated this run?"}
    G2b -- no --> Skip
    G2b -- yes --> G3{"Still inside the<br>warmup window?"}
    G3 -- yes --> Skip
    G3 -- no --> G4{"Within this site's first<br>N eligible calls (skip_first)?"}
    G4 -- yes --> Skip
    G4 -- no --> G5{"Injection budget<br>already spent?"}
    G5 -- yes --> Skip
    G5 -- no --> Draw["<b>Draw once</b> from this<br>class's sub-stream"]
    Draw --> Cmp{"select a FaultKind<br>from this site's rule"}
    Cmp -- no --> Lost["<b>Not fired</b><br>drew = yes"]
    Cmp -- yes --> Fired["<b>Fired</b><br>drew = yes"]

    Skip --> Rec["Write ledger record<br><i>status + reason + drew</i>"]
    Lost --> Rec
    Fired --> Rec
    Rec --> Out(["return FaultKind<br><i>wrapper maps it to a legal API result</i>"])
```

Three things this diagram encodes:

- **The ordering is not stylistic.** Every gate that exits *before* the draw is what makes Rule 3 hold, and Rule 3 is what lets you change one setting without disturbing everything else.
- **The eligible-call counter has one exact definition.** A call becomes *eligible* the moment it has passed the quiet, class-enabled, site-activated, and warmup gates (G1–G3); the per-site eligible counter increments at that point — **before** the `skip_first`, trigger, and budget checks. `skip_first = N` therefore means "the first N eligible calls never fire", and `fire_on_eligible_call = K` means "the K-th eligible call fires deterministically". Because both key off the same counter, `validate()` rejects a rule with `fire_on_eligible_call ≤ skip_first` (the trigger could never fire). The budget gate (G5) comes last, so an exhausted budget blocks even a matched deterministic trigger.
- **Ledger recording is selective, not total** (§11.1): fires and limit-refusals are recorded as entries; routine gate rejections only advance per-site counters. The `drew` flag distinguishes "returned before the draw" from "drew and lost" — the distinction the F5 decision trace needs to realign the RNG stream without guessing.

### 10.1 Deterministic Pattern Triggers (Tier 1)

The gate chain above always ends the same way: a random draw decides whether a fault fires. That is exactly right for broad, undirected exploration — but it is the wrong tool for reproducing one *specific* scenario on demand, such as a regression test that says "fail exactly the 443rd allocation" or "refuse the 3rd reconnection attempt, every time, regardless of seed." Waiting for the swarm to stumble into that exact situation is unreliable and slow.

**A deterministic trigger is a fact, not a coin flip.** It replaces the draw with a direct check, so it never touches the RNG at all:

```text
quiet? → class on? → site on? → warmup? → skip_first passed?
                                              │
                     (all gates passed — eligible_calls[site] += 1)
                                              │
                     eligible_calls[site] == fire_on_eligible_call?
                                     │                        │
                                   yes                        no
                                     │                        │
                              budget left?                     │
                                 │       │                    │
                                yes      no                   │
                                 │       │                    │
                       FIRE, drew = no   Skipped      [Draw once, as before]
```

This is the same idea the Linux kernel's fault-injection framework already uses for `space`/`times` (§16 ref. 5), narrowed from *a window* of eligible calls down to *one exact occurrence*.

**Tier 1 scope: occurrence-count triggers on wrapper sites.** Each generic `FaultRule` names its site through the `rules` map and may specify the exact eligible-call number that should fire:

```cpp
FaultRule rule;
rule.outcomes = {{FaultKind::OutOfMemory, 1.0}};
rule.fire_on_eligible_call = 443;  // 1-based: fire on this site's 443rd eligible call
cfg.rules.emplace(SiteId::malloc, std::move(rule));
```

The counter it checks is the **eligible-call count** as defined in §10 — incremented for calls that passed quiet/class/site/warmup, *before* `skip_first` and budget are checked — not the raw, unfiltered call count. This applies uniformly to every wrapper site: allocation, I/O, network, and clock.

It does **not** apply to process/topology faults. There is no `crash()` symbol in the application's binary for the linker to wrap, so there is no "eligible call" to count — an occurrence trigger on `site::crash_node` is a category error, and `validate()` rejects it (§12). Episode faults get their own deterministic trigger form in §10.2.

**Ledger entry:** identical shape to any other decision, with a distinct reason so it's never confused with a probabilistic fire:

```text
t=61ms   Memory   site=malloc   FIRED   drew=no   reason=trigger_matched(eligible_call=443)
```

Because `drew=no`, this follows the same rule that already governs every other zero-draw path (§7 Rule 3): firing deterministically must never shift the RNG stream, or a scenario that's supposed to be scripted and exact would start perturbing unrelated probabilistic faults in the same run.

**Why this is worth having on its own, independent of the swarm:** it turns the workflow into "declare the exact scenario, then assert the expected recovery" instead of "hope chaos eventually produces it":

```cpp
FaultConfig cfg;
cfg.scheduled_episodes.push_back({.at = 61ms, .spec = CrashNode{n2}, .until_heal = 2s});

scenario.check("recovers-from-single-crash", replicas_agree_after_recovery());
```

(The crash itself is a §10.2 scheduled episode — occurrence triggers are for wrapper sites; virtual-time triggers are for episodes.)

**Deferred to a later version — Tier 2 and beyond.** Tier 1 asks only "has this wrapper site been reached N times?" (§10.1) and "has virtual time T arrived?" (§10.2). A more expressive version — worth designing later, once Tier 1 has proven useful — would let a trigger track a short *sequence* of events (a small per-trigger state machine, e.g. "fire on `site B` only if `site A` fired within the last 20ms and no heal has happened since"), and a further tier beyond that could match arbitrary predicates over live simulation state (which node is currently leader, whether a write is in flight). Both add real state and cost to the injector — a state machine to advance per trigger, or read access to node/clock state it doesn't currently need — so they are intentionally out of scope until Tier 1's simpler, cheaper mechanism is shown to be insufficient in practice.

### 10.2 Event Sites & Virtual-Time Triggers (Tier 1b)

Episode faults (crashes, partitions) are not reached by application calls, so neither the gate chain's occurrence counting nor its per-call draw applies to them. They are triggered two ways, both fully deterministic and both **zero-draw** (Rule 11):

1. **Imperative, from the harness**: `injector.crash_node(n2, 2s)` — the scripted style already used for `sim.crash(...)` in `docs/design.md` §9.
2. **Scheduled, from the config**: `FaultConfig::scheduled_episodes` is a list of `(at, spec, until_heal)` triples. The engine arms one virtual-time event per entry at universe start; when the event fires, the injector attempts the episode start.

The attempt is still subject to the **fault-model limits** (§2) and the **quiesce window** (§9.3): if starting the episode would break the promise, it is refused and recorded as `Skipped` with the limit's reason — a scheduled crash is not exempt from the building code. Because no draw is involved, `drew = no`.

Event sites carry `SiteId`s (`site::crash_node`, `site::partition`) so that swarm activation can disable an entire event site for a universe, and so ledger and trace entries use the same identity scheme as wrapper sites. There is no `rate` on an event site: it fires when invoked or scheduled, subject to limits, or it does not fire at all. Probabilistic *timing* of an episode ("crash at a random moment") is expressed by sampling the `at` field from the `Swarm` stream during config sampling — randomness enters at sampling time, not at fire time, so the runtime decision stays zero-draw.

---

## 11. The Fault Ledger, Decision Trace & Minimization

### 11.1 As a report

The ledger is the **diagnostic** record: it exists so a human (and the coverage checks of §11.4) can see what the application was subjected to. It records every **fired** fault, every heal, and every **limit-refused** episode start. Routine gate rejections (class disabled, site inactive, warmup, `skip_first`, budget spent) are **not** recorded as entries — a malloc-heavy run makes millions of eligible calls, and a ledger of skips would drown the signal in noise and tax every run for data it never reads. They advance per-site counters (`eligible_calls`, `outcomes_fired`) instead, and the counters are what the coverage checks read.

(The complete every-decision record needed for replay is a separate artifact — the **decision trace** of §11.2, introduced at sprint F5. Keeping the two apart is deliberate: the ledger answers "what happened?", the trace answers "how do I re-run it?". Conflating them forces every pre-F5 run to pay trace-recording cost it cannot use.)

Each entry carries an explicit status:

| Status | Meaning |
|---|---|
| `Fired` | The fault was injected (or an episode healed) and the application observed it. |
| `Skipped` | An episode start was **refused by a fault-model limit** (§2) or landed inside the quiesce window — the promise would have been exceeded. Routine gate rejections are *counted*, not recorded. |
| `Suppressed` | A replay deliberately withheld a fault that the original run fired (§11.2). Appears in trace-driven replay runs only, from sprint F5. |

Each entry also records **whether a random draw was consumed** (`drew: yes/no`) — `yes` for probabilistic fires, `no` for deterministic-trigger fires, heals, and refusals. The flag becomes load-bearing in the decision trace (§11.2), where a replay cannot tell a "returned early, stream untouched" decision from a "drew and lost the coin flip" decision without it — and those leave the RNG in different states.

```text
Run 8421 (FAILED: "no split-brain") — fault ledger:
  t=12ms   Memory   site=malloc      FIRED    drew=yes  alloc #443 → ENOMEM
  t=30ms   Network  site=partition   FIRED    drew=no   partition {n0} | {n1,n2}
  t=45ms   Network  site=partition   FIRED    drew=no   partition healed (scheduled heal)
  t=58ms   Process  site=crash_node  SKIPPED  drew=no   reason=max_crashed_nodes (limit refusal)
  t=61ms   Process  site=crash_node  FIRED    drew=no   node n2 crashed

Per-site counters:  malloc eligible=12,004 fired=1 · send eligible=873 · write eligible=402 · …
```

This alone is valuable: it shows exactly what the application was subjected to, **and what it was deliberately spared**, before it broke — and the counters prove how often each site was actually exercised (§11.4).

### 11.2 As a replay input — the part that must be designed in early

Phase 5 wants **minimization**: a failing run injected 47 faults, but probably only 2 or 3 actually mattered. The other 44 are noise. Minimization re-runs the seed while suppressing faults one at a time — still fails without fault #3? Then #3 was irrelevant; drop it. Repeat until only the essential faults remain.

For that to work, the decision trace must be an **editable recipe**, not just a receipt.

> **Design constraint:** a fault's identity must be **stable across replays**. It cannot be "the 7th draw from the memory stream", because suppressing an earlier fault renumbers everything after it and the scheme collapses.

#### Why a live occurrence counter is not enough

The obvious identity is `(class, site_id, occurrence_index)`, where `occurrence_index` counts encounters of that site during the run. **That is not stable**, for the same reason the naive version isn't: suppressing an earlier fault changes the application's control flow. It retries fewer times, allocates fewer times, and reaches the site a different number of times — so the 5th encounter in the replay is not the 5th encounter of the original run.

The identity must therefore come from the **recorded decision schedule**, not from live execution. The trace is **one globally ordered list**, not per-site queues:

- The original run writes a **decision trace**: an ordered list of `(seq, virtual_time, site_id, eligible_index, outcome, drew)` entries, in the order the decisions happened. `seq` is a strictly increasing sequence number; `eligible_index` is the eligible-call count at record time — informational only, never recomputed during replay.
- A replay walks the trace with a **single cursor**. At each live decision point it peeks at the cursor entry:
  - **Same site** → consume the entry and honour it: apply its outcome (or withhold it, if the entry was rewritten to `Suppressed`), and if `drew = yes`, consume exactly one draw from that class's sub-stream *even when the outcome is suppressed* — the stream must advance exactly as the original run's did, or every later honoured entry receives a different random value.
  - **Different site, or trace exhausted** → control flow has diverged from the original run (or run past its end). The call **passes through with no draw and no fault** (Rule 12). Replay never invents faults that were not in the original trace — otherwise a minimization re-run could "still fail" because of a fault that never existed in the failing run, and the reduction would prove nothing.
- Suppression rewrites one entry's outcome to `Suppressed` and leaves every other entry untouched.

**Why global order, and not a per-site queue:** sites within one fault class *share* the class sub-stream (Rule 2). The original run's draws from e.g. the `Memory` stream are interleaved across `malloc`/`calloc`/`realloc` in one specific order, and that order is part of the stream's state. Consuming entries per-site could hand a draw intended for a `calloc` decision to a `malloc` decision. The single cursor preserves the original per-class draw order exactly, for as long as the replayed execution tracks the original one.

**What replay guarantees, stated honestly:**

1. **Replay is deterministic.** `replay(trace, seed)` is a pure function: cursor position, stream positions, and application behaviour are all fully determined by the trace and the seed. Two replays of the same trace produce bit-identical traces.
2. **Pre-divergence, replay is identical to the original run** — same decisions, same draws, same outcomes.
3. **Post-divergence, alignment is best-effort.** Once suppression changes control flow, later trace entries may no longer line up with live calls. The no-novel-faults rule keeps the replay honest; it does not pretend to reconstruct the original run's RNG alignment, because no scheme can.

This makes identity a property of the *recorded schedule*, which suppression edits deliberately, rather than of the *replayed execution*, which suppression perturbs as a side effect. Episode triggers (§10.2) are recorded in the trace as zero-draw entries at their virtual timestamps; since scheduled episodes are armed at universe start, they re-fire at the same virtual times in replay, as long as the replayed run's clock reaches them — one of the few things divergence cannot easily move.

#### What minimization actually produces

One-at-a-time reduction yields a **1-minimal** set: no *single* remaining fault can be removed without losing the failure. It is **not** necessarily the globally smallest set. When faults interact — say the bug needs *either* (A and B) *or* (C and D) — removing any one of the four still reproduces via the other pair, so all four survive the reduction.

Reporting a 1-minimal set is the honest and standard outcome; a genuinely minimal set requires searching subsets (delta debugging's `ddmin`), which is a later optimisation. The report should say which guarantee it is offering.

### 11.3 As a determinism check

The decision trace feeds the FNV-1a trace hash used by `--verify` double-run validation (`docs/design.md` §15); before F5, the diagnostic ledger's canonical form fills the same role.

**The hash is computed over a canonical serialization, never over in-memory layout.** Struct padding, field ordering, native endianness, pointer values, and `double` formatting all vary across compilers, flags, and platforms — hashing raw object bytes would make `--verify` fail for reasons that have nothing to do with determinism. The canonical encoding fixes:

| Aspect | Canonical form |
|---|---|
| Integers | Fixed-width, **little-endian**, explicit widths (`u64`, `u32`, `u8`) |
| Timestamps | `int64` **nanoseconds** of virtual time — never a formatted string |
| Enums (`FaultClass`, status) | Their explicit `uint8_t` value, never their name |
| Site IDs | `u64`, stable across builds |
| Strings (reasons) | `u32` byte length, then raw UTF-8 |
| Rates / doubles | **Excluded** from the hash — they belong to the config, which is reported separately, and their text form is platform-dependent |
| Field order | The declared order of the ledger record, with no padding emitted |

Records are hashed in trace order (`seq` ascending), each field appended in that fixed encoding.

### 11.4 As proof the faults actually fired

If a configured fault rate is `0.001` and a run reaches only 200 eligible sites, most runs inject nothing at all and the test is silently vacuous. A campaign-level coverage check such as `sometimes(fault_was_injected(FaultClass::Storage), "storage fault path exercised")` proves the configuration is genuinely doing work — and it is backed by the per-site counters of §11.1, not by manual log inspection. This is Antithesis's primary recommended use for `sometimes`.

---

## 12. Public API Reference

```cpp
namespace cosmos {

enum class FaultClass : uint8_t { Memory, Network, Storage, Clock, Process, Random, _Count };
enum class FaultMode  : uint8_t { Safety, Liveness };

/// Stable identity for every injection site (§6.2). APPEND-ONLY: never renumber
/// or reuse an ID — activation sets, ledgers, traces, and repro commands all
/// key on these values (Rule 13). This document's `site::X` shorthand means
/// `SiteId::X`.
enum class SiteId : uint64_t {
    // Wrapper sites (point faults, decided via FaultInjector::decide)
    malloc = 1, calloc, realloc,
    open, read, write, fsync,
    connect, accept, send, recv,
    clock_gettime, nanosleep,
    getrandom,
    // Event sites (episode faults, §10.2) — never passed to decide()
    crash_node = 1000, partition, pause_node,
};

/// What the wrapper should make the call do. `None` = pass through unchanged.
/// The wrapper — never the injector — maps each kind to a legal API result
/// (§8.2), e.g. OutOfMemory → nullptr + ENOMEM, WriteEio → -1 + EIO.
enum class FaultKind : uint8_t {
    None = 0,
    OutOfMemory,
    OpenEio, ReadEio, WriteEio, ShortWrite, NoSpace, FsyncEio,
    ConnRefused, ConnReset, PeerClose, ShortSend,
    PacketDrop, PacketDelay, PacketReorder, PacketCorrupt,
    ClockStep, SleepInterrupted,
    RandomEagain,
    _Count,          // sentinel; new kinds append before it
};

/// Rule 15, enforced at config time: an outcome must be one the site's own API
/// could really produce, so `send -> ENOMEM` is rejected by validate() rather
/// than discovered as a false-positive finding later.
constexpr bool is_legal_outcome(SiteId, FaultKind);

struct SiteOutcome {
    FaultKind kind;
    double    weight;    // relative weight; > 0; any scale ({5,3,2} == {0.5,0.3,0.2})
};
// Inline fixed-capacity storage, not a vector: a config copy must not allocate, because the
// engine is reached from inside __wrap_malloc. Four is the widest menu any site has (send).
// The widest menu is send(): reset, short send, drop, delay, reorder, corrupt.
// A static_assert ties this to is_legal_outcome(), so the cap can never silently
// be smaller than the outcomes a site is allowed to name.
constexpr size_t kMaxOutcomes = 6;
struct OutcomeTable {
    std::array<SiteOutcome, kMaxOutcomes> entries;
    size_t count;                                // >= 1 when the rule can fire
};

/// Rules are generic: the injector chooses a FaultKind and the wrapper maps it
/// to a legal result for its API (ENOMEM, EIO, ECONNRESET, a short write, ...).
struct FaultRule {
    double rate = 0.0;                       // finite and in [0, 1]
    uint64_t skip_first = 0;                 // first N *eligible* calls (§10) never fire
    uint64_t max_injections = UINT64_MAX;    // cap on non-None outcomes; also blocks triggers
    OutcomeTable outcomes;                   // mutually exclusive; exactly one per call
    // Tier-1 occurrence trigger (§10.1). Wrapper sites only — event sites use
    // FaultConfig::scheduled_episodes instead (§10.2); validate() rejects it there.
    std::optional<uint64_t> fire_on_eligible_call;
};

// ---- Supporting types ----
using NodeId            = uint32_t;
using NodeSet           = std::vector<NodeId>;
using EpisodeId         = uint64_t;
using KnobId            = uint64_t;   // named per app tunable; append-only, like SiteId
/// Every site occupies a pinned slot: wrapper sites fill [0, kWrapperSiteCount), event sites
/// follow. site_slot() is written out rather than derived from the enum value, so retiring a site
/// never shifts the slots after it (Rule 13). Slot order — not hash order — is what the swarm
/// sampler iterates and draws against.
constexpr size_t kWrapperSiteCount = 14;
constexpr size_t kEventSiteCount   = 3;
constexpr size_t kSiteCount        = kWrapperSiteCount + kEventSiteCount;
constexpr size_t site_slot(SiteId);              // total; kNoSite for an unknown value

using SiteActivationSet = std::bitset<kSiteCount>;
using SiteCounterMap    = std::array<uint64_t, kSiteCount>;

/// An episode's identity: what breaks, and on whom (§4.2).
struct CrashNode { NodeId id; };
struct Partition { NodeSet a, b; };
struct PauseNode { NodeId id; };
using EpisodeSpec = std::variant<CrashNode, Partition, PauseNode>;

struct ScheduledEpisode {
    Time        at;
    EpisodeSpec spec;
    Duration    until_heal;    // every episode schedules its own heal (Rule 5);
                               // persistent faults use start_persistent() instead
};

enum class ConfigError : uint8_t {
    BadRate, BadWeight, BadOutcomeKind, IllegalOutcome, EmptyOutcomes,
    TriggerLeSkipFirst, TriggerOnEventSite, RuleOnEventSite,
    RuleOnDisabledClass, EpisodeOutsideWindows, BadEpisodeDuration,
    UnknownNode, BadKnobOrder, QuorumExceedsNodes, LimitsExceedNodes,
    BadWindowOrder,
};

/// Which site the config was wrong about, not just what was wrong with it.
struct ConfigProblem { ConfigError error; std::optional<SiteId> site; };

/// Pure data. Sampled once per universe from the seed. Copyable and printable,
/// so a failing run can report the exact configuration that produced it.
struct FaultConfig {
    FaultMode mode = FaultMode::Safety;

    // Swarm level 1a: which fault classes are active in this universe at all.
    std::bitset<static_cast<size_t>(FaultClass::_Count)> enabled;

    // Swarm level 1b: which individual sites are activated (§6.2). Absent entry
    // means "not activated". Populated only for classes that are enabled.
    SiteActivationSet activated_sites;

    // Generic per-site rules, indexed by site_slot(). A simple site has one
    // non-None outcome; a multi-outcome site has several. Every wrapper uses
    // this same mechanism.
    //
    // Fixed storage rather than a hash map, for two reasons. Iteration order is
    // load-bearing: the swarm sampler draws per site, and an unordered
    // container's order is specified nowhere — it varies across standard library
    // implementations and versions — so the same seed could produce different
    // configs on different machines, breaking Rules 4 and 8 and the (build, seed)
    // repro contract. And the rules array never allocates, so the decision path
    // and a rules-only config copy cannot re-enter __wrap_malloc (Rule 7);
    // scheduled_episodes and knobs are vectors and do allocate, so a config
    // carrying either is not allocation-free to copy. Event-site slots exist so
    // that a rule wrongly placed on one is representable, and therefore
    // rejectable by validate().
    std::array<std::optional<FaultRule>, kSiteCount> rules;

    // Fault-model limits: never exceed what the application promises to survive.
    uint32_t max_crashed_nodes  = 0;
    uint32_t min_healthy_quorum = 0;

    // Deterministic episode triggers (§10.2): armed at universe start, fired
    // zero-draw at their virtual times, still subject to the limits below.
    std::vector<ScheduledEpisode> scheduled_episodes;

    // Knob faults (§4.3): extreme-but-legal tuning values, sampled once per
    // universe. The harness delivers them to the app (env/CLI/config) before
    // the app starts; static for the whole run (Rule 14).
    // Strictly ascending by id, checked by validate(): sampling draws per knob,
    // so the order is part of the reproduction contract exactly as `rules` is.
    std::vector<Knob> knobs;

    // Run lifecycle windows.
    Time warmup_until  = Time::zero();
    Time quiesce_after = Time::max();

    /// Level 1 swarm sampler. Draws from the dedicated `Swarm` stream, using a
    /// fixed class-indexed draw schedule that covers disabled classes too
    /// (§6.4), so the swarm dimensions stay independent.
    /// Postcondition: validate() passes.
    static FaultConfig sample(Rng& swarm_rng);

    /// Rejects configs that would silently misbehave:
    ///   - every rule rate must be finite and in [0, 1];
    ///   - a rule with rate > 0 or a trigger must have >= 1 outcome; every
    ///     outcome weight must be > 0 and finite (weights are relative, so any
    ///     positive scale is legal; normalize() rescales them to sum to 1);
    ///   - every outcome kind must be one its site's API could really produce
    ///     (Rule 15), and never FaultKind::None;
    ///   - a scheduled episode must name real nodes and have a nonzero duration;
    ///   - max_crashed_nodes + min_healthy_quorum must fit within the node count,
    ///     or the config contradicts itself;
    ///   - fire_on_eligible_call must be > skip_first (else it can never fire);
    ///   - occurrence triggers are rejected on event sites (§10.2);
    ///   - rules and triggers are rejected for sites whose class is disabled;
    ///   - scheduled episodes must lie inside [warmup_until, quiesce_after);
    ///   - min_healthy_quorum must not exceed the node count;
    ///   - warmup_until must be <= quiesce_after.
    /// Reports the first problem found, in a fixed order: whole-config limits,
    /// then sites by ascending slot, then episodes, then knobs. Within a site:
    /// placement, class, rate, outcomes, trigger. So a NaN rate on a disabled
    /// class reports RuleOnDisabledClass, not BadRate.
    /// Enforced in sample() and again in the FaultInjector constructor.
    /// node_count is a parameter rather than a field: cluster size is topology,
    /// not fault configuration. The error carries the offending site.
    [[nodiscard]] std::expected<void, ConfigProblem> validate(uint32_t node_count) const;

    /// Rescales every outcome table to sum to 1, once, the way
    /// std::discrete_distribution does at construction. Separate from validate()
    /// so a checker never rewrites the config it was asked to inspect; the
    /// injector calls it on its own copy.
};

/// Stateful engine. Owns the per-class RNG sub-streams, the budgets, the quiet
/// windows, the active-episode registry, the per-site counters, and the ledger.
/// Never leaks into the user's mental model.
class FaultInjector {
  public:
    /// Episode faults need more than config and randomness: they need to know
    /// what time it is, what is already broken, and how to schedule their own
    /// heal. Those dependencies are injected explicitly rather than reached for
    /// via a global, so the injector stays unit-testable in isolation.
    FaultInjector(FaultConfig cfg,
                  Rng fault_stream,
                  const VirtualClock& clock,   // read virtual now
                  EventQueue& events,          // schedule automatic heals
                  const NodeRegistry& nodes);  // query live/crashed state for limits

    // ---- Point faults: one gate chain, at most one draw (§10). ----
    // Wrapper sites only — event sites fire via scheduled_episodes and the
    // episode API below, never through decide(). Non-const: drawing mutates.
    // FaultKind::None means pass the original call through unchanged. Otherwise
    // the caller's wrapper translates the result to its API-specific failure.
    FaultKind decide(FaultClass cls, SiteId site);

    // ---- Episode faults ----
    // Checks the fault-model limits against live node state, starts the episode,
    // registers it as active, and schedules its heal on the event queue.
    // Returns the handle so a caller may heal it early; nullopt means the start
    // was refused (a Skipped ledger entry is written with the reason).
    std::optional<EpisodeId> start_partition(const NodeSet& a, const NodeSet& b,
                                             Duration until_heal);
    std::optional<EpisodeId> crash_node(NodeId id, Duration until_reboot);

    // Persistent faults (§4.2.1): deliberately created without a heal event.
    std::optional<EpisodeId> start_persistent(FaultClass cls, PersistentSpec spec);

    void heal(EpisodeId id);
    void heal_all();                 // used by quiesce and the Restore fault

    // ---- Mode transition (§3) ----
    // One-way. Force-heals every fault touching a core node and converts each
    // remaining non-core episode into a persistent fault.
    void enter_liveness_mode(const NodeSet& core);

    // ---- Quiet windows ----
    void push_quiet();          // engine-internal or app-requested critical section
    void pop_quiet();
    void begin_quiesce();       // terminal settle-down: heal_all() + faults off

    const FaultConfig& config() const;
    const FaultLedger& ledger() const;

  private:
    FaultConfig  cfg_;
    std::array<Rng, static_cast<size_t>(FaultClass::_Count)> streams_;  // Rule 2

    const VirtualClock& clock_;      // borrowed, outlives the injector
    EventQueue&         events_;
    const NodeRegistry& nodes_;

    ActiveEpisodeMap active_;        // what is currently broken
    int              quiet_depth_{0};
    bool             quiescing_{false};
    SiteCounterMap   eligible_calls_;    // calls that passed quiet/class/site/warmup (§10)
    SiteCounterMap   injections_;        // non-None outcomes per site
    FaultLedger      ledger_{};
};

/// RAII helper for quiet windows. Non-copyable and non-movable: a copy would
/// call pop_quiet() twice for a single push_quiet(), silently re-enabling
/// faults inside a critical section.
class QuietGuard {
  public:
    explicit QuietGuard(FaultInjector& fi) : fi_(fi) { fi_.push_quiet(); }
    ~QuietGuard() { fi_.pop_quiet(); }

    QuietGuard(const QuietGuard&)            = delete;
    QuietGuard& operator=(const QuietGuard&) = delete;
    QuietGuard(QuietGuard&&)                 = delete;
    QuietGuard& operator=(QuietGuard&&)      = delete;

  private:
    FaultInjector& fi_;
};

} // namespace cosmos
```

**Why `FaultConfig` and `FaultInjector` are separate types:**

1. `FaultConfig` is what the *user* writes and reads.
2. `FaultConfig` is what gets *printed* in a failure report (`seed=8421, mode=Safety, site=write, rate=0.003`), which makes findings self-describing.
3. `FaultInjector` holds mutable engine state (RNG position, counters, quiet depth, active episodes) that must never appear in the user's mental model.

A single fused struct forces the decision method to be `const` while it needs to mutate an RNG — the tension visible in the current scaffolded `FaultProfile`.

**Why the injector takes clock, event queue, and node registry:** point faults need only config and randomness, but episode faults cannot be enforced without them. Checking `max_crashed_nodes` requires knowing which nodes are *currently* down; scheduling an automatic heal requires the event queue; stamping a ledger entry requires virtual time. Passing them in keeps the rule "every episode schedules its own heal" enforceable by construction rather than by convention.

### 12.1 The harness surface: `FaultPlan`, `Scenario`, and `cosmos::run`

`FaultPlan` (used in §17) is the harness-facing name for a `FaultConfig` authored for one scenario — a type alias, not a new type. `Scenario` is the minimal harness driver; it lives in the harness layer, not in the injector:

```cpp
using FaultPlan = FaultConfig;

class Scenario {
  public:
    explicit Scenario(FaultPlan plan);
    void run(std::function<void()> workload);                    // execute with faults ON
    void quiesce();                                              // heal_all + drain + settle (§9.3)
    void check(std::string id, std::function<bool()> oracle);    // always-style (§12 of design.md)
    void note_covered(std::string id, bool hit);                 // sometimes-style (§11.4)
};
```

`cosmos::run` (the §17 teaser) is pure sugar over the pieces above: `cosmos::run({.seed = S, .oom = {.fail_on_call = K}}, workload, oracle)` builds a `FaultConfig` whose only rule is `SiteId::malloc → { outcomes = {OutOfMemory}, fire_on_eligible_call = K }`, runs warmup → workload → quiesce → oracle in one universe, and prints the ledger on failure. It exists so the smallest useful test is one expression; anything richer drops down to `Scenario` or `Campaign`.

### 12.2 Migration note: `FaultProfile` is superseded

The scaffolded `FaultProfile` (`oom_rate` + `should_inject_oom(Rng&)` in `include/cosmos/faults.hpp`) is replaced by `FaultConfig` / `FaultRule` / `FaultInjector`. The split exists precisely because `FaultProfile` fused user-facing config with engine state: the scaffold's intermediate-rate decisions currently draw from the Simulator's Memory sub-stream via `should_inject_oom(Rng& rng)` (endpoint rates consume no draw, §7 Rule 3), and sprint F2 moves that decision into `FaultInjector::decide(FaultClass::Memory, SiteId::malloc)` with full rule/timeline support. `wrap_memory.cpp` is re-pointed at that call site in sprint F2, `FaultProfile` is deleted, and the `FaultProfile` mentions in `docs/design.md` §4/§9 are updated to match. What survives from the current header: `FaultClass` and `fault_class_seed` (§7 Rule 2's per-class sub-stream derivation), which the new design keeps unchanged.

### 12.3 What Phase 1 ships, and how it differs from the reference above

The declarations in §12 describe the **F6-complete** injector. Phases 1–5 ship strict subsets, so reading §12 against the code turns up differences that are planned sequencing rather than drift. This table is the reconciliation; a row leaves it when the sprint in its last column lands. A difference still listed here after that sprint is real drift and should be fixed.

| §12 reference | What the code has today | Closed by |
|---|---|---|
| `FaultInjector(cfg, Rng, VirtualClock&, EventQueue&, NodeRegistry&)` | `BasicFaultInjector<ClockLike>`, built by `create(cfg, fault_stream_seed, node_count, clock)` | F6: event queue + node registry |
| Constructor enforces `validate()` | `create()` returns `std::expected<BasicFaultInjector, ConfigProblem>`; the constructor is private, so there is no unchecked path | — the factory is the permanent shape |
| `const VirtualClock&` | Templated on a `ClockLike` concept (anything answering `now()`). `include/cosmos/virtual_clock.hpp` is a placeholder pending the runtime clock | F6: replacing that header, with no change to the injector |
| Categorical walk shown inline in `decide()` | Split into a pure `constexpr FaultKind select_outcome(double, const FaultRule&)`; `decide()` still takes exactly one `uniform()` per call | — permanent; see below |
| Occurrence triggers (`fire_on_eligible_call`) | Not wired. `create()` rejects any config that sets one, with a temporary `ConfigError::TriggerNotImplemented` | F1: wiring the trigger deletes that enum member |
| Episodes, ledger, `begin_quiesce()`, mode transitions | Absent | F1 (ledger), F6 (episodes, limits, mode) |

**Why `select_outcome` is a separate function.** `uniform()` returns multiples of 2⁻⁵³, so a draw landing exactly on a rate or on a cumulative-weight boundary is a 1-in-2⁵³ event that no seed will produce. Those boundaries are precisely where an off-by-one hides, and they cannot be reached by sampling. Extracting the value-to-outcome half as a pure function lets the boundaries be pinned with `static_assert` at exact values, which makes a regression a compile error rather than a test that may never fire. The behaviour is unchanged and the one-draw-per-call rule of §6.5 is untouched — `decide()` still owns the draw and the injection counter.

**One conformance note on the walk.** `normalize()` divides by the weight total, so the walk's cumulative sum can stop a hair below 1, and a draw landing in that gap returns `None` — a fire is lost. The pseudocode in §6.5 has the same property, so the implementation is conformant rather than buggy. The gap is the per-fire loss probability, and it is asserted to stay within a few ULPs, which bounds the loss at roughly one fire in 10¹⁵ rather than leaving it unquantified.

---

## 13. Determinism Rules (Summary)

| # | Rule | What breaks if violated |
|---|---|---|
| 1 | Faults draw only from the `fault` stream | Changing fault rates silently changes thread interleavings |
| 2 | Each fault class gets its own sub-stream | Changing network settings shifts memory faults; impossible to vary one thing at a time |
| 3 | Never draw **at runtime** for a fault that cannot fire | Turning a fault *off* changes unrelated results |
| 4 | Never draw inside an address-order loop | ASLR leaks non-determinism back in |
| 5 | Episode faults always schedule their own heal; only persistent faults (§4.2.1) may omit one | Recovery behaviour becomes untestable, or a missing heal cannot be told from an intended one |
| 6 | Fault identity comes from the recorded decision trace, not a live counter | Minimization (Phase 5) cannot be built |
| 7 | Engine-internal allocations are never faulted | The simulator corrupts itself; all results invalid |
| 8 | **Config sampling** uses a fixed class-indexed draw schedule, including disabled classes | Swarm dimensions become correlated; toggling one class shifts another's rate |
| 9 | The ledger is recorded for every decision, with status and `drew` flag | Replay cannot distinguish "returned early" from "drew and lost" |
| 10 | The trace hash covers a canonical encoding, never in-memory layout | `--verify` fails across compilers and platforms for non-determinism reasons |
| 11 | A deterministic trigger (§10.1 occurrence, §10.2 virtual-time) firing never consumes a draw | A scripted, exact scenario would perturb unrelated probabilistic faults sharing the same run |
| 12 | Replay of a fixed decision trace never invents new faults: on trace mismatch or exhaustion, eligible calls pass through with no draw | Minimization re-runs get confounded by faults that never existed in the original run (§11.2) |
| 13 | `SiteId` (and `KnobId`) enumerations are append-only; IDs are never renumbered or reused | Historical repro commands, ledgers, and traces silently re-point at the wrong sites |
| 14 | Knob values are sampled once per universe, delivered by the harness before the app starts, and never mutated mid-run | A mid-run knob change is an unrecorded episode fault the ledger cannot explain (§4.3) |
| 15 | An injected result must be legal for its API, including invariants (e.g. `CLOCK_MONOTONIC` never steps backward) | The app is tested against an impossible world; findings are false positives (§8.2) |

---

## 14. Testing the Injector Itself

The injector is the one component where a silent bug invalidates **every** result the tool ever produces. It needs its own test suite, roughly in order of value:

| Test | What it proves |
|---|---|
| **Determinism** — same seed twice ⇒ identical ledger | The basic promise holds |
| **Stream isolation** — change *network* config, assert the *memory sub-stream* yields the same value sequence and the same per-eligible-allocation decisions | Rule 2 actually works. Highest-value test in the list — note it compares **decisions**, not ledger bytes (§7 Rule 2) |
| **Replay with suppression** — suppress fault #k, all other decision-trace entries are honoured identically | The ledger is a valid replay input; minimization is buildable |
| **Rate calibration** — rate `0.01` over 100k trials lands in statistical bounds | Catches `<` vs `<=` and bad uniform conversion |
| **Gate coverage** — no fault ever fires during warmup, quiet, or quiesce | Lifecycle windows hold |
| **Swarm coverage** — over N sampled configs, every class is enabled at least once | The swarm sampler is not silently ignoring a class |
| **Fault-model limits** — never exceeds `max_crashed_nodes`; every refusal is recorded as `Skipped` with the limit's reason | No false-positive findings |
| **No-draw gates** — with a fresh injector, run every gate-blocking configuration and assert the class sub-stream position never advances | Rule 3 in its direct, measurable form |
| **Trigger no-draw** — a matched occurrence trigger and a fired scheduled episode both leave every sub-stream position unchanged | Rule 11 |
| **Trigger/budget interplay** — an exhausted budget blocks even a matched trigger; `validate()` rejects `fire_on_eligible_call ≤ skip_first` | §10's counter semantics are exact, not approximate |
| **Legality** — a `ClockStep` on `CLOCK_MONOTONIC` never moves time backward; every mapped `FaultKind` produces a documented API result | Rule 15; no impossible-world findings |
| **Quiesce completeness** — after `begin_quiesce()`, no fault fires, every episode (persistent ones included) is healed, and the ledger shows the heals | §9.3; final invariants observe a recovered system |
| **Replay exhaustion** (F5) — consuming a trace past its end or at a mismatched site passes through with no draw and no new faults; two replays of one trace are bit-identical | Rule 12; minimization is trustworthy |

The stream-isolation and replay-with-suppression tests are the non-obvious ones, and they are the two that pay off most later.

---

## 15. Implementation Roadmap

The order below is deliberately MVP-first: prove that one real fault, injected deterministically, can be caught by one real check, before spending any effort on breadth (more fault classes) or depth (swarm sampling, minimization, distributed faults). Each later sprint only starts once the sprint before it is genuinely solid — none of them are worth doing early against a shaky foundation.

| Sprint | Content | Exit criteria |
|---|---|---|
| **F0** ✅ | **Seeded RNG** (`random.hpp`): `xoshiro256**` + `splitmix64` derivation, the five domain streams, per-class sub-streams, universe-seed derivation from `(campaign_seed, index)` | **Done** — known-answer tests pass against published reference vectors (`tests/test_random.cpp`, `tests/test_seed_derivation.cpp`). |
| **F1** | Generic `FaultRule` / `FaultInjector::decide` split; `validate()`; stable append-only `SiteId`; gate chain with exact eligible-counter semantics (§10), quiet windows, budgets, and deterministic occurrence triggers (§10.1); the v1 diagnostic ledger: fires + limit refusals + per-site counters (§11.1) | A configured rule fires on its exact eligible occurrence with `drew=no`; invalid rates/outcome tables/triggers are rejected; gate-blocked calls provably never advance the RNG streams |
| **F2** | First adapter plus correctness-oracle surface (§17): `__wrap_malloc` → `OutOfMemory`; a minimal `Scenario`/`FaultPlan` harness with `quiesce()` and `check()`; `FaultProfile` deleted (§12.2) | A single seed reproducibly fails a deliberately broken example app, and passes once the break is fixed — proves one fault, one adapter, and one oracle end to end |
| **F3** | Generic wrapper-surface expansion, one call at a time: `calloc`/`realloc`; `open`/`read`/`write`/`fsync` (`EIO`, short write, `ENOSPC`); `send`/`recv`/`connect` (`ECONNRESET`, 0-byte close, delayed delivery); clock reads — each reusing F1's decision engine unchanged | Every newly wrapped call maps a named `FaultKind` to a documented legal API result and is independently tested |
| **F4** | Campaign runner (many seeds, `never_hit` tracking); swarm sampler (`FaultConfig::sample`) with fixed class-indexed draws + per-site activation; mutually-exclusive categorical draw for multi-outcome sites (§6.5) | Stream-isolation and swarm-coverage tests pass; a multi-outcome site never produces more than one outcome per call |
| **F5** | Decision trace recording; replay with suppression (no novel faults after divergence — Rule 12); 1-minimal minimization; canonical trace encoding + FNV-1a trace hash | Suppressing one fault leaves every other honoured trace entry bit-identical; two replays of one trace are bit-identical; `--verify` stable across compilers |
| **F6** | Distributed faults: virtual clock, event queue, node registry, episode lifecycle (every episode schedules its own heal — Rule 5), persistent faults, scheduled episode triggers (§10.2), fault-model limits with recorded refusals, `Liveness` mode transition | Every episode heals or is explicitly persistent; liveness assertions expressible on the distributed example |

**Not planned:** automatically inserting faults into application logic without the application author writing anything (§8.3) — this isn't a later sprint, it's a capability this design deliberately doesn't claim.

F0 and F1 are deliberately small. One fault class implemented properly — with its gate chain, ledger, and isolation tests solid — makes every later class nearly free. Adding classes before that skeleton is right multiplies the rework. Everything from F4 onward (breadth via swarm, minimization, distributed faults) is real value, but none of it is needed to prove the architecture works — F0 through F3 alone already deliver a usable tool.

The executable, checkpointed version of this roadmap — broken into phases and sprints with per-sprint happy/sad-path checkpoints — lives in **`SPRINT_PLAN.md`**, kept outside the repository as an uncommitted working document.

---

## 16. References

Sources studied to derive this design.

### 1. FoundationDB — `BUGGIFY` and Swarm Knobs
* **Docs**: [FoundationDB Client Testing](https://apple.github.io/foundationdb/client-testing.html)
* **Analysis**: [Diving into FoundationDB's Simulation Framework — Pierre Zemb](https://pierrezemb.fr/posts/diving-into-foundationdb-simulation/)
* **Taken from it**: the two-level probability model (`section_activated_probability` = 25%, `section_fired_probability` = 25%); knob randomisation (a 60s production timeout becoming 0.1s in simulation); in-code injection sites reaching logic that library wrapping cannot.

### 2. Swarm Testing — Groce, Zhang, et al. (ISSTA 2012)
* **Paper**: [Swarm Testing (PDF)](https://agroce.github.io/issta12.pdf)
* **Taken from it**: the justification for per-run configuration diversity. A swarm of configurations that each *omit* some features found **42% more distinct crashes** in C compilers than a hand-tuned single configuration. Two mechanisms: features that actively suppress bugs, and features competing for room within a test.

### 3. TigerBeetle VOPR — Fault Model and Liveness Mode
* **Blog**: [Simulation Testing for Liveness](https://tigerbeetle.com/blog/2023-07-06-simulation-testing-for-liveness/)
* **Docs**: [VOPR internals](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/internals/vopr.md)
* **Taken from it**: the separation of Safety mode (unbounded chaos) from Liveness mode (force-heal a quorum "core", make non-core faults permanent, then demand progress); the principle that faults must stay inside the declared fault model or every finding is a false positive.

### 4. Antithesis — Fault Taxonomy, Restore, and Terminal Pause
* **Docs**: [Fault injection overview](https://antithesis.com/docs/concepts/fault_injection/) · [Types of faults](https://antithesis.com/docs/product/fault_injection/fault_types/) · [Sometimes assertions](https://antithesis.com/docs/best_practices/sometimes_assertions/)
* **Taken from it**: faults interleaved continuously with the workload rather than staged; the explicit `Restore` fault that clears all active network faults; the terminal pause (`eventually` / `finally`) giving the system time to recover before final validation; `sometimes` assertions as proof that fault injection actually fired.

### 5. Linux Kernel — Fault Injection Framework
* **Docs**: [Fault injection capabilities infrastructure](https://www.kernel.org/doc/html/latest/fault-injection/fault-injection.html)
* **Taken from it**: the battle-tested parameter set — `probability`, `interval`, `times` (maximum injections), `space` (skip the first N). Adopted generically in each `FaultRule` as `rate`, `max_injections`, and `skip_first`.

### 6. Jepsen
* **Site**: [jepsen.io](https://jepsen.io/analyses)
* **Taken from it**: the catalogue of real distributed-systems failure shapes that informs which faults are worth injecting at all.

---

## 17. Worked Example: Specifying, Injecting, and Diagnosing

The sections above define the machinery in isolation: the fault model, the probability engine, the injection sites, the ledger. This section walks the same 3-node replicated KV store used as the running example in §2 through the **full loop**, end to end — because the individual pieces only make sense once you see how a user actually touches them.

**The loop, in one picture:**

```mermaid
flowchart LR
    A["1. Write the promise<br><i>FaultConfig limits</i>"] --> B["2. Write the oracle<br><i>Scenario check in harness</i>"]
    B --> C["3. Run the campaign<br><i>many seeds, faults ON</i>"]
    C --> D{"Any harness check<br>failed?"}
    D -- no --> E["Pass — but check<br>coverage wasn't vacuous"]
    D -- yes --> F["Failure report:<br>seed + ledger + timestamp"]
    F --> G["4. Read the ledger<br>backward from the failure"]
    G --> H["5. Minimize:<br>replay, suppress one fault at a time"]
    H --> I["Minimal fault set:<br>the actual repro"]
```

Before the full walkthrough, here's the same loop at its smallest — one exact fault, one check, nothing else:

```cpp
cosmos::run(
    {.seed = 8421, .oom = {.fail_on_call = 443}},   // the promise: fail exactly the 443rd allocation
    [&] { app.run_workload(); },                     // the workload
    [&] { CHECK(app.is_consistent()); });             // the oracle
```

This alone is already the whole point proven end to end: a seed reliably reproduces one specific failure, and one check says whether the app survived it. Everything else in this section — more faults, more seeds, richer oracles — is the same shape scaled up.

There is no separate step for "define the expected state" versus "define the fault state" — steps 1 and 2 below together **are** that definition, expressed as code rather than as a template. And critically, **none of that code lives inside the application.** The application's own source stays exactly what its author wrote (§8) — every piece below lives in a separate test harness that starts the real application and talks to it only the way any other client would: through its public API, and by reading whatever state it actually persists.

Three things the harness needs to say, matching the three concerns from §2 and §12:

| Concern | Who states it | Where |
|---|---|---|
| What can break | The fault model limits (§2) | A `FaultPlan`, built from `FaultConfig` |
| What the test actually does | Ordinary calls into the app's public client | A workload function |
| What must still be true afterward | A check against public state, persisted state, or a small reference model | An oracle |

### 17.1 Step 1 — Declare the promise

This is the fault model from §2, written as data, and packaged as a `FaultPlan` — the harness-facing name for a `FaultConfig` built for one scenario:

```cpp
FaultPlan plan;
plan.enabled.set(FaultClass::Network);
plan.enabled.set(FaultClass::Process);

plan.max_crashed_nodes  = 1;   // the promise: survive losing any 1 of 3 nodes
plan.min_healthy_quorum = 2;

plan.mode = FaultMode::Safety; // unbounded chaos within the limits above (§3)
```

This *is* the "template" for the fault side of the question. It is not free-form — `max_crashed_nodes` and `min_healthy_quorum` are the only two numbers the injector checks before starting an episode fault (§2), so the promise is exactly as expressive as those two fields and no more. That is intentional: a promise that's harder to state than "N nodes, M quorum" usually means the fault model itself needs to be re-thought, not that the config needs more knobs.

### 17.2 Step 2 — Drive the app, then check what it left behind

This is the "expected state," and it's checked from **outside** the application, against things any real client or operator could also see. There are three practical ways to write a check, in increasing strength:

| Style | What it checks | Good for |
|---|---|---|
| **A. Public API** | The app's own client-facing answers, e.g. "is this write still marked complete?" | Fast, simple, close to what a real caller sees |
| **B. Persisted state** | What's actually on disk/replicated after the run, read independently of the app's own code | Storage and consistency properties |
| **C. Reference model** | A small model the harness keeps itself ("I told it to transfer 100 from a to b"), compared against the app's state at the end | The strongest check, for properties an API answer alone can't confirm |

```cpp
Scenario scenario{.faults = plan};

Cluster cluster = start_replicated_kv_under_test();   // the real app, unmodified, running for real

scenario.run([&] {
    client.put("user:42", "value");                    // workload — ordinary client calls
    client.retry_until_acknowledged("user:42");
});

scenario.quiesce();   // stop injecting, heal everything, let the app finish recovering (§9.3)

// Oracle, style B — read persisted state directly, not through app-internal hooks.
scenario.check("no-committed-data-loss", [&] {
    return read_committed_value(cluster, "user:42") == "value";
});
scenario.note_covered("node-crash-path-exercised", a_process_fault_fired_this_run);
```

`scenario.quiesce()` is doing exactly the work described in §9.3 — checking "did every replica keep the write" is only a fair question once the network has actually had a chance to heal, so the harness calls it explicitly before checking anything.

### 17.3 Step 3 — Run the campaign

The scenario from §17.2 runs once per seed, unchanged — a campaign is just this same harness repeated:

```cpp
CampaignConfig ccfg;
ccfg.trials    = 5000;
ccfg.base_seed = 1;

CampaignReport report = Campaign::run(ccfg, [&](Simulator& sim, uint64_t universe_index) {
    Scenario scenario{.faults = FaultConfig::sample(sim.swarm_rng())};
    run_replicated_kv_scenario(scenario);   // the exact harness from §17.2, one seed at a time
});
```

Each of the 5000 universes gets its own seed (§6.4), its own swarm-sampled rates (§6), and runs the full lifecycle from §9. The user does not watch any of this run — they read `report` afterward:

```text
report.runs        = 5000
report.failed_runs = 3
report.never_hit    = []   // sometimes("node-crash-path-exercised") did fire — not vacuous
```

3 failures out of 5000 is the signal. Everything else was a clean pass under real chaos.

### 17.4 Step 4 — Read the failure

One of the three failures prints:

```text
FAILED: "no-committed-data-loss"
  seed    = 8421
  detail  = "node=n1 key=user:42"
  t       = 87ms

Fault ledger:
  t=12ms   Memory   site=malloc      FIRED    drew=yes  alloc #443 → ENOMEM
  t=30ms   Network  site=partition   FIRED    drew=no   partition {n0} | {n1,n2} (scheduled episode)
  t=45ms   Network  site=partition   FIRED    drew=no   partition healed (scheduled heal)
  t=61ms   Process  site=crash_node  FIRED    drew=no   node n2 crashed (scheduled episode)
  t=87ms   ← check "no-committed-data-loss" FAILED here
```

The user does not manually diff two states. The failing harness check **is** the detection: after quiesce it compared the independently observed state with the promised result. Diagnosis is reading the ledger *backward from `t=87ms`*: the network partition at `t=30ms` isolated `n1` right before `n2` crashed at `t=61ms`, which is the obvious suspect — `n1` likely committed a write it believed was replicated to `n2`, but `n2` never actually received it before crashing, and nothing else held a durable copy.

That is a hypothesis, not proof. With only 4 faults in this short ledger it is easy to eyeball, but a long run can have dozens, and eyeballing stops working. That's what step 5 automates.

### 17.5 Step 5 — Minimize to the actual cause

```text
cosmos --seed 8421 --minimize
```

This replays seed 8421 repeatedly, each time suppressing one ledger entry (§11.2) and checking whether `"no-committed-data-loss"` still fails:

```text
suppress t=12ms (malloc OOM)         → still FAILS → OOM was irrelevant, drop it
suppress t=30ms (partition start)    → PASSES      → this fault was essential, keep it
suppress t=61ms (n2 crash)           → PASSES      → this fault was essential, keep it
```

Result — a 1-minimal reproduction (§11.2):

```text
Minimal fault set for "no-committed-data-loss":
  t=30ms   Network   partition {n1} isolated from {n0,n2}
  t=61ms   Process   n2 crashed while still isolated

Repro: cosmos --seed 8421 --only-faults=2   (the two entries above, nothing else)
```

This is the actual bug report a developer acts on: *"if `n1` is partitioned away from the cluster right when `n2`, the only other holder of a given write, crashes, the write is lost — even though only 1 of 3 nodes ever died, which the fault model says should be survivable."* That is a genuine violation of the promise from §2, not a false positive — the injector never exceeded `max_crashed_nodes = 1`, so this finding is real.

### 17.6 What this example does and does not show

This walkthrough uses only mechanisms already specified earlier in this document — §2's limits, §9's phases, §11's ledger and minimization, and the `Scenario`/`Campaign` harness. It does not introduce a new "template" format, because the existing mechanism already covers the need: **the promise is data (`FaultConfig`), the check is harness code (`Scenario::check`), and the two meet in the scenario result.** A declarative template would only be worth adding later if real usage shows the same small oracle patterns being copy-pasted across many applications; it would be a helper library on top of this mechanism, not a change to it.
