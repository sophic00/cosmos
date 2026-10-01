# Future: Substrate Seam & Snapshot Branching

**Status: `wont-do` — [Phase 7](roadmap.md#unified-roadmap).**

This content was moved out of [`architecture.md`](architecture.md) because it describes a
substrate abstraction that the codebase does not have. There is no `ISubstrate`, no
`SimSubstrate`, no `KvmSubstrate`, and no `Snapshot` type anywhere in the repository. Keeping it
in the architecture document made that document describe a system that does not exist.

It is preserved here because the idea is coherent and the seam is worth keeping in view. Nothing
in the current engine is built on it. Do not implement this without first editing the
[roadmap](roadmap.md#unified-roadmap) row.

---

## 1. Why a substrate seam was proposed

Today the determinism engine and the execution mechanism are the same thing: `BasicSimulator`
drives ucontext fibers inside the test process, and determinism comes from single-threaded
cooperation plus `--wrap` interposition. That couples two concerns:

- the **determinism machinery** — seeded RNG, virtual clock, fault decisions, ledger, campaign
  runner, replay;
- the **thing being executed** — in-process C/C++ fibers today, potentially a full VM guest later.

The seam would let the first be reused unchanged by the second.

---

## 2. The proposed interface

```cpp
class ISubstrate {
public:
    virtual ~ISubstrate() = default;
    virtual NodeId create_node(std::string_view name) = 0;
    virtual void   crash(NodeId id) = 0;
    virtual void   reboot(NodeId id) = 0;
    virtual void   run_until(Time deadline_or_quiescence) = 0;
    virtual void   deliver_packet(NodeId target, Packet packet) = 0;
    virtual Snapshot save_snapshot() = 0;
    virtual void     restore_snapshot(Snapshot&& snap) = 0;
};
```

Two implementations were imagined:

1. **`SimSubstrate`** — executes C++ fiber tasks inside the single-threaded simulation process.
   Determinism is maintained by single-threaded cooperation and wrapper interposition.
2. **`KvmSubstrate`** — executes full virtual machine guests (Linux + KVM). Intercepts hypercalls
   and VM exits for unmodified, multi-process guest-OS determinism. Would reuse the campaign
   runner, fault timeline, and exploration engine unchanged.

---

## 3. Snapshot interface & multiverse branching

State-space exploration v3 would fork a universe at critical decision points to explore
alternative interleavings without restarting from time zero.

```cpp
struct Snapshot {
    uint64_t virtual_time_ns;
    uint64_t rng_state[4];
    std::vector<uint8_t> heap_state;
    std::vector<Packet> inflight_packets;
};
```

- **`SimSubstrate`** would create an in-process state copy of the tracked heap, virtual clock, and
  network queues.
- **`KvmSubstrate`** would perform hypervisor copy-on-write memory snapshot and vCPU register
  state save.

Two preconditions are unmet in the current code: the RNG state (`random.hpp`) is private with no
save/restore, and the ledger has no serialization path. Both are listed against
[F5](roadmap.md#unified-roadmap).

---

## 4. Why this is not being built

- **It would require virtual dispatch in wrapper paths.** Every `__wrap_*` call is on the
  per-call hot path and inside the reentrancy discipline described in
  [`architecture.md`](architecture.md#6-invariants). A virtual call there is both a determinism and
  a performance decision that the current design avoids on purpose.
- **There is no second substrate to validate it against.** An abstraction with one
  implementation is a guess at what the second one will need. The repository's stated scope for
  the seam is "reuses 100% of the campaign runner", which cannot be tested until the earlier
  roadmap rows exist — see [`roadmap.md`](roadmap.md#unified-roadmap).
- **The current engine is not blocked on it.** Phases 2 and 3 (simulated network, campaign
  runner) are ordinary code inside `libcosmos`.

**Explicit non-goal:** no virtual dispatch in wrapper paths without a second substrate to justify
it. See [`architecture.md`](architecture.md#5-seams) for the seams that do exist today.

---

## 5. Where the idea surfaces in the current design

Worth knowing so future work does not accidentally foreclose it:

- `BasicSimulator<Injector>` is a template rather than a concrete class, and the `Injector` is
  already a seam of the same shape (`HasFactory`, `ClockLike` — see
  [`architecture.md`](architecture.md#5-seams)).
- The `__wrap_*` / `__real_*` pair is already the boundary at which a different execution
  mechanism would attach.
- `FaultConfig`, `FaultRule`, and `SiteId` are pure data with no reference to a simulator type,
  so a fault timeline is already substrate-agnostic.
