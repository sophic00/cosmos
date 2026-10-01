# Cosmos

Cosmos is an embeddable C++ library for **Deterministic Simulation Testing (DST)** for C/C++ applications via standard POSIX library function interposition (`-Wl,--wrap`).

It provides zero-code-change simulation testing for standard POSIX functions (`malloc`, `free`, `pthread_create`, `clock_gettime`, `socket`, `send`, `recv`, `open`, `write`, `fsync`, `getrandom`).

## Documentation

Full documentation is available in the [`docs/`](docs/) directory.

**Start with [`docs/roadmap.md`](docs/roadmap.md)** — it is the single source of truth for what is
built, what is partly built, and what is deliberately not being built. Every other document links
there instead of restating a status.

| Document | What it covers |
|---|---|
| [Roadmap & Known Defects](docs/roadmap.md) | Unified phase/sprint status table and the register of defects the shipped code has |
| [Architecture](docs/architecture.md) | Structure, the build-flag interposition layer, seams, and the load-bearing invariants |
| [Design Specification](docs/design.md) | Public API reference and per-subsystem designs, each with a status line |
| [Fault Injection](docs/fault-injection.md) | Fault model, probability and swarm design, gate chain, ledger, and worked example |
| [Determinism Contract](docs/determinism-contract.md) | What is guaranteed, user obligations, determinism rules, build rules, boundaries |
| [Writing a Wrapper](docs/writing-a-wrapper.md) | How to add a POSIX function to the interposition layer |
| [Testing](docs/testing.md) | Build conventions, the Scenario lifecycle, and what each test target proves |
| [Implementation Notes](docs/IMPLEMENTATION_NOTES.md) | What `-Wl,--wrap` actually reaches, and where it structurally cannot |
| [Linker Interposition](docs/linker-interposition.md) | The `-Wl,--wrap` / `__real_*` mechanism and toolchain citations |
| [Future: Substrate Seam](docs/future-substrate.md) | The `ISubstrate` / snapshot idea, demoted because none of it is implemented |
| [Project Plan](docs/plan.md) | Original goals, POSIX taxonomy, and design decisions |
| [Antithesis Study Notes](docs/antithesis-study-notes.md) | DST research and prior art |

## Quick Start

Using `just` or `make`:

```bash
# Production build
just build       # or make build

# Simulation build (builds simulation binaries only)
just sim         # or make sim (or just s)

# Build and run tests
just test        # or make test (or just t)


# Build all targets (production binaries, simulation examples, and tests)
just all         # or make all

# Format codebase & check formatting
just format      # or make format
just format-check # or make format-check
just lint        # or make lint


# Clean build artifacts
just clean       # or make clean
```

Or using CMake directly:

```bash
# Configure and build libcosmos, tests, and simulation examples
cmake -B build -DCOSMOS_BUILD_TESTS=ON
cmake --build build

# Run unit tests
ctest --test-dir build --output-on-failure

# Run simulation examples directly
./build/examples/single_node/kv_store_sim
./build/examples/distributed/replicated_kv_sim
```

## Examples

> **Status: the two headline examples are placeholders.** `kv_store.c` and `replicated_kv.c` are
> short `main()` functions that print a banner. Their earlier descriptions claimed a transactional
> WAL storage engine and a replicated consensus cluster; neither exists. Crash durability and
> network partition/reordering cannot yet be demonstrated, because the storage page-cache model
> and the simulated network are not implemented — see
> [`docs/roadmap.md`](docs/roadmap.md#unified-roadmap).

- [`examples/single_node/`](examples/single_node/) – placeholder. The real single-node fixture in
  the tree is [`broken_cache.c`](examples/single_node/broken_cache.c), a small key-value cache
  with a deliberate missing-`malloc`-check defect, driven by `test_broken_app` / `test_fixed_app`
  to demonstrate finding a bug by seed and verifying the fix.
- [`examples/distributed/`](examples/distributed/) – placeholder. Cannot use sockets under a
  simulation: the socket wrappers abort by design.
