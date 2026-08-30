add_library(cosmos STATIC
    cosmos.cpp
    wrappers/wrap_memory.cpp
    wrappers/wrap_threads.cpp
    wrappers/wrap_time.cpp
    wrappers/wrap_net.cpp
    wrappers/wrap_storage.cpp
    wrappers/wrap_random.cpp
)

target_include_directories(cosmos PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Each __wrap_* symbol must live in its own linker section so that the sim
# binary's -Wl,--gc-sections can discard wrappers for POSIX functions the
# application never calls. This is what makes SUBSET wrapping work: pulling
# __wrap_malloc must not drag in __wrap_calloc (and its __real_calloc ref),
# which would otherwise be an undefined symbol unless --wrap=calloc is also
# passed. See docs/design.md §2 (build matrix) and docs/architecture.md §3.
target_compile_options(cosmos PRIVATE -ffunction-sections -fdata-sections)

# GCC/Clang treat malloc/free as builtins with known semantics and delete pairs they can prove
# are dead, so at -O1 and above the call never reaches __wrap_malloc and the allocation is
# silently untracked. INTERFACE so every binary linking cosmos inherits it, not just this library.
target_compile_options(cosmos INTERFACE -fno-builtin-malloc -fno-builtin-free)
