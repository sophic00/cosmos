# Antithesis - Study Notes & Interposition Comparison

Background research for Cosmos. Sources: Antithesis engineering blog, product pages, and technical documentation (July 2026).

Although Cosmos is a library-first deterministic simulation testing platform (using linker symbol interposition `-Wl,--wrap`), these notes capture the foundational research on deterministic execution: input trees, seeded exploration, property assertions, snapshot branching, time travel, and interposition models.

---

## Table of Contents

1. [Interposition Models: Hypervisor vs. Linker Wrapping](#1-interposition-models-hypervisor-vs-linker-wrapping)
2. [The Determinator (Antithesis Deterministic Hypervisor)](#2-the-determinator-antithesis-deterministic-hypervisor)
3. [Topology & Isolation](#3-topology--isolation)
4. [Time Determinism (The Crux)](#4-time-determinism-the-crux)
5. [Deterministic I/O Boundary](#5-deterministic-io-boundary)
6. [Snapshots & The Multiverse](#6-snapshots--the-multiverse)
7. [Exploration & Fault Injection](#7-exploration--fault-injection)
8. [dhyve (Open-Source Proof of Concept)](#8-dhyve-open-source-proof-of-concept)
9. [Prior Art Summary](#9-prior-art-summary)

---

## 1. Interposition Models: Hypervisor vs. Linker Wrapping

Deterministic Simulation Testing (DST) requires trapping and controlling all sources of nondeterminism (time, randomness, concurrency, I/O). There are three primary levels of interposition:

| Mechanism | Interposition Level | App Changes | C++ Header Safety | Overhead | Best Suited For |
|---|---|---|---|---|---|
| **Linker Wrapping (`-Wl,--wrap`)** | Linker / Symbol level | **Zero** (Standard POSIX C/C++) | ✅ **100% Safe** | Zero in Prod | In-process C/C++ libraries & systems (Cosmos library substrate) |
| **Hypervisor VMCALL / Intercept** | Hardware / VM level | **Zero** (Unmodified OS VM image) | ✅ **100% Safe** | Microsecond VM traps | Unmodified guest OS binaries, multi-process clusters (Antithesis / Cosmos [Phase 7](roadmap.md#unified-roadmap)) |
| **Dynamic `LD_PRELOAD`** | Dynamic Loader level | **Zero** | ✅ **100% Safe** | Minimal | Dynamic library override (has bootstrap/init ordering traps) |

Cosmos adopts **Linker Symbol Wrapping (`-Wl,--wrap`)** as its primary substrate seam, enabling zero-code-change DST for C/POSIX applications without VM overhead.

---

## 2. The Determinator (Antithesis Deterministic Hypervisor)

- Fork of FreeBSD **bhyve**, Intel VMX. First move: **remove** functionality: start from a small deterministic core and grow incrementally. Minimal device surface is their #1 design lesson.
- Work = categorizing every micro CPU behavior as *deterministic* (keep in guest) or *nondeterministic* (avoid/contain/reverse).

---

## 3. Topology & Isolation

- The **entire system under test** (all containers, Docker Compose / K8s) runs inside **one VM**, pinned to **one physical core**. Parallelism = many VMs exploring different branches; never multi-vCPU within a VM (inter-core interleaving kills instruction-level determinism).
- The unit of reproducibility is the **whole interconnected system state**, not a single process.

---

## 4. Time Determinism (The Crux)

- Rule: **the guest clock must be a function of only the deterministic state and execution history of the guest.**
- All time sources (TSC/RDTSC, HPET, etc.) are intercepted, and virtual time is returned.
- Open-source **dhyve** uses a **retired branch instruction count** to track execution progress deterministically.

---

## 5. Deterministic I/O Boundary

- One controlled channel (custom `VMCALL` hypercall): logs/assertions out, commands + RNG seeds in.
- **Every point where the guest consumes external input is a possible branch point.** A run is an **input tree**; exploration walks/forks that tree. (Cosmos equivalent: the seed-derived decision streams).

---

## 6. Snapshots & The Multiverse

- Fast full-guest snapshots: never replay from the beginning.
- Enables: branch-the-past exploration; rewind-inspect; retroactive debugger attach; time compression of idle periods.
- Cosmos analogue: `Snapshot` save/restore with in-process COW memory copies ([Phase 5](roadmap.md#unified-roadmap)).

---

## 7. Exploration & Fault Injection

- Extreme chaos engineering inside the deterministic bubble; fuzzed inputs + feedback-guided exploration seeking novel states.
- Detection = **user-defined properties** via SDK: `always` (invariant per run), `sometimes` (evaluated campaign-wide across all runs).

---

## 8. dhyve (Open-Source Proof of Concept)

- DTU bachelor project (2 students, one semester): bhyve fork with branch-count virtual time; intercepted randomness/timers/I/O; snapshot/branch/mutation harness. Proof that a minimal DST platform is achievable with student-scale effort.

---

## 9. Prior Art Summary

- **FoundationDB Simulation**: The original in-process DST (Flow language), single-threaded deterministic simulator, simulated network/disk/time, swarm testing.
- **TigerBeetle VOPR**: Deterministic simulator + strict state-machine design + heavy assertions.
- **rr**: Process-level record/replay via retired-conditional-branches counter and chaos scheduling.
- **Jepsen**: Catalog of distributed-systems bug shapes that informs fault taxonomies and property verification.
