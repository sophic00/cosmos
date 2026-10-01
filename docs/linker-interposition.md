# Linker Symbol Interposition (`-Wl,--wrap`) in Cosmos

This document explains how function address resolution is routed through the DST middle layer via linker symbol interposition (`-Wl,--wrap`), the `__real_*` passthrough mechanism, and toolchain mechanics.

---

## 1. Overview & Core Concept

Cosmos executes unmodified C/POSIX applications deterministically by intercepting standard POSIX library symbols (`malloc`, `clock_gettime`, `pthread_create`) at final link time, avoiding custom source-level runtime abstractions.

```mermaid
flowchart TD
    App["Application Source Code<br/><i>calls malloc()</i>"] -->|"Linker rewrites symbol"| Wrap["__wrap_malloc() in libcosmos"]
    Wrap --> Sim["Virtual Sim Heap / Fault Injection"]
    Sim -->|"calls __real_malloc()"| LinkerRewriter["Linker Symbol Resolution"]
    LinkerRewriter --> Libc["Real System libc malloc()"]
```

---

## 2. Linker Symbol Resolution Mechanics (`-Wl,--wrap`)

The `-Wl,--wrap=<symbol>` linker option provided by GNU `ld` and LLVM `lld` performs symbol table transformations at final executable link time:

1. **`symbol` ➔ `__wrap_symbol`**: Undefined references to `symbol` across all input object files and linked static libraries are rewritten to resolve to `__wrap_symbol`.
2. **`__real_symbol` ➔ `symbol`**: Undefined references to `__real_symbol` are rewritten to resolve directly to the original, unwrapped `symbol` definition (e.g., from `libc` or `libpthread`).

### Synthetic Symbol Resolution (`__real_*`)
`__real_symbol` routines are synthetic symbols created by the linker; they are never defined in source code or headers. Wrapper files specify an `extern "C"` forward declaration (e.g., `extern "C" void* __real_malloc(size_t);`) strictly to satisfy frontend compiler type checking. At link time, the linker resolves references to `__real_symbol` by aliasing them directly to the underlying system library symbol.

---

## 3. Illustration, and where the real code is

The snippets below show the **mechanism**. They are deliberately minimal and are not the
repository's actual wrapper code — the real wrappers add a reentrancy guard, a null-universe
check, an eligibility predicate, and a fault decision. For the real thing see
[`wrap_memory.cpp`](../src/cosmos/wrappers/wrap_memory.cpp) and the walkthrough in
[`architecture.md`](architecture.md#43-a-wrapper-for-real). For the full boundary analysis see
[`IMPLEMENTATION_NOTES.md`](IMPLEMENTATION_NOTES.md).

### A. The shape of a wrapper

```cpp
#include <cstddef>

extern "C" {

// Forward declaration of the linker-synthesized real symbol alias:
void* __real_malloc(size_t size);

// Interposition wrapper called whenever application code calls malloc():
void* __wrap_malloc(size_t size) {
    // 1. Intercept call (apply virtual heap tracking, OOM fault injection, etc.)
    // 2. Delegate to the real system allocator via __real_malloc:
    return __real_malloc(size);
}

} // extern "C"
```

### B. The shape of the build configuration

```cmake
add_executable(myapp_sim myapp.c)
target_link_libraries(myapp_sim PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(myapp_sim PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=clock_gettime"
        "-Wl,--wrap=write"
    )
endif()
```

The repository's real lists are much longer — [`examples/single_node/CMakeLists.txt`](../examples/single_node/CMakeLists.txt)
passes **34** wrap flags to `kv_store_sim`, covering the memory, time, storage, random, and
pthread families. Every consumer target carries its own list, because subset wrapping is per
target.

---

## 4. Official Toolchain Citations

### 1. GNU Linker (`ld` / Binutils)
* **Documentation**: [GNU `ld` Manual — Command-Line Options](https://sourceware.org/binutils/docs/ld/Options.html)
* **Option**: `--wrap=symbol`
* **Official Specification**:
  > *"Use a wrapper function for `symbol`. Any undefined reference to `symbol` will be resolved to `__wrap_symbol`. Any undefined reference to `__real_symbol` will be resolved to `symbol`.*
  > 
  > *This can be used to provide a wrapper which checks the arguments, or provides default values, or logs the call. The wrapper function should be called `__wrap_symbol`. If it wishes to call the real function, it should call `__real_symbol`."*

### 2. LLVM Linker (`lld` / Clang)
* **Documentation**: [LLVM `lld` Command Line Reference](https://lld.llvm.org/) / `man lld`
* **Behavior**: LLVM `lld` implements full drop-in specification parity for `--wrap=<symbol>`. References to `symbol` are rewritten to `__wrap_symbol`, and references to `__real_symbol` resolve to the unwrapped symbol `symbol`.

### 3. GCC & Clang Compiler Frontends (`-Wl,`)
* **Documentation**: [GCC Manual — Options Controlling Linking](https://gcc.gnu.org/onlinedocs/gcc/Link-Options.html)
* **Option**: `-Wl,option`
* **Official Specification**:
  > *"-Wl,option : Pass option as an option to the linker. If option contains commas, it is split into multiple options at the commas."*

---

## 5. Technical Considerations & Best Practices

Three flags cooperate to make interposition reliable. All three are required; the third is the
one most often missed.

1. **Link-Time Garbage Collection (`-ffunction-sections` + `-Wl,--gc-sections`)**:
   In Cosmos ([`src/cosmos/CMakeLists.txt`](../src/cosmos/CMakeLists.txt)), each `__wrap_*` symbol is built with `-ffunction-sections`. This ensures that if a target binary wraps a subset of symbols (e.g. `malloc` but not `calloc`), `-Wl,--gc-sections` discards unused `__wrap_*` functions and prevents undefined references to un-wrapped `__real_*` symbols.
2. **Same Translation Unit Direct Calls**:
   If a function call and definition exist in the same translation unit, compilers may optimize calls directly without generating an undefined symbol reference in the object file relocations. Interposition via `--wrap` applies strictly to calls resolved across translation units / object boundaries.
3. **Compiler Builtins (`-fno-builtin-X`)**:
   This is the flag most likely to be forgotten, and its absence is silent. For a function with a
   compiler builtin — `malloc`, `free`, `calloc`, `realloc`, `random`, `rand`, `srand`,
   `srandom`, among others — the optimiser may fold the call into an intrinsic, inline it, or
   prove it redundant. When that happens **no undefined reference is emitted at all**, so there is
   nothing for `--wrap` to rewrite. The wrapper is compiled, linked, present in the binary, and
   never called.

   Cosmos handles this in [`src/cosmos/CMakeLists.txt`](../src/cosmos/CMakeLists.txt) by adding
   `-fno-builtin-malloc`, `-fno-builtin-free`, `-fno-builtin-calloc`, `-fno-builtin-realloc`,
   `-fno-builtin-random`, `-fno-builtin-rand`, `-fno-builtin-srand`, `-fno-builtin-srandom` — and
   propagating them as an `INTERFACE` option so that any consumer of `libcosmos` inherits them and
   cannot silently lose interposition by forgetting the flag.

   When adding a wrapper for a function that has a builtin, add its `-fno-builtin-X` too. See
   [`writing-a-wrapper.md`](writing-a-wrapper.md) §5.

   Note that `-ffunction-sections` and `--gc-sections` solve a *different* problem — dropping
   wrappers you did not ask for. `-fno-builtin-X` solves the problem of the wrapper you *did* ask
   for never being reached. Both symptoms look like "my wrapper is not running".

For the complete list of what `--wrap` structurally cannot reach — fortified glibc variants such
as `__read_chk`, `operator new` under a shared `libstdc++` link, raw syscalls, and unwrapped POSIX
families — see [`IMPLEMENTATION_NOTES.md`](IMPLEMENTATION_NOTES.md).
