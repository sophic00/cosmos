# Tests assert() their expectations, so NDEBUG would silently compile every check away and
# report a vacuous pass. CMake caches CMAKE_BUILD_TYPE across `just build` and `just test`,
# so a prior Release configure would otherwise leak NDEBUG into the test binaries.
add_compile_options(-UNDEBUG)

# The campaign worker pool runs on std::thread; the platform pthread flag must reach it.
find_package(Threads REQUIRED)

add_executable(test_wrap_malloc test_wrap_malloc.cpp)
target_link_libraries(test_wrap_malloc PRIVATE cosmos)
# White-box test: pins the internal memory eligibility policy from wrapper_fault.hpp.
target_include_directories(test_wrap_malloc PRIVATE ${CMAKE_SOURCE_DIR}/src/cosmos/wrappers)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Linker --wrap substitutes malloc/free AFTER optimization, but GCC/Clang
    # treat them as builtins with no observable side effects. Under -O2 the
    # optimizers cache/reorder loads of sim-heap bookkeeping across the wrapped
    # calls, and clang folds out-of-bounds header reads as UB. -fno-builtin
    # makes the wrapped symbols opaque calls again. See docs/linker-interposition.md.
    target_compile_options(test_wrap_malloc PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
    target_link_options(test_wrap_malloc PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
    )
endif()

add_test(NAME test_wrap_malloc COMMAND test_wrap_malloc)

add_executable(test_random test_random.cpp)
target_link_libraries(test_random PRIVATE cosmos)
add_test(NAME test_random COMMAND test_random)

add_executable(test_seed_derivation test_seed_derivation.cpp)
target_link_libraries(test_seed_derivation PRIVATE cosmos)
add_test(NAME test_seed_derivation COMMAND test_seed_derivation)

add_executable(test_time test_time.cpp)
target_link_libraries(test_time PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_time PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
    )
    target_link_options(test_time PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=clock_gettime"
        "-Wl,--wrap=gettimeofday"
        "-Wl,--wrap=nanosleep"
        "-Wl,--wrap=clock_nanosleep"
    )
endif()

add_test(NAME test_time COMMAND test_time)

add_executable(test_faults test_faults.cpp)
target_link_libraries(test_faults PRIVATE cosmos)
add_test(NAME test_faults COMMAND test_faults)

add_executable(test_faultinjection_phase1_gate test_faultinjection_phase1_gate.cpp)
target_link_libraries(test_faultinjection_phase1_gate PRIVATE cosmos)
add_test(NAME test_faultinjection_phase1_gate COMMAND test_faultinjection_phase1_gate)

add_executable(test_ledger test_ledger.cpp)
target_link_libraries(test_ledger PRIVATE cosmos)
add_test(NAME test_ledger COMMAND test_ledger)

add_executable(test_fault_draw test_fault_draw.cpp)
target_link_libraries(test_fault_draw PRIVATE cosmos)
add_test(NAME test_fault_draw COMMAND test_fault_draw)

add_executable(test_fault_injector test_fault_injector.cpp)
target_link_libraries(test_fault_injector PRIVATE cosmos)
add_test(NAME test_fault_injector COMMAND test_fault_injector)

add_executable(test_simulator test_simulator.cpp)
target_link_libraries(test_simulator PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Same reason as test_wrap_malloc above: the wrapped symbols must stay opaque calls.
    # calloc/realloc are wrapped too: the simulator verify-shape test exercises all four
    # allocators, and an unwrapped realloc would hand a tracked (header-offset) pointer to the
    # host allocator and abort.
    target_compile_options(test_simulator PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
    target_link_options(test_simulator PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
    )
endif()

add_test(NAME test_simulator COMMAND test_simulator)

add_executable(test_assert test_assert.cpp)
target_link_libraries(test_assert PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # record_finding materializes std::strings through the wrapped heap when a universe is
    # current, so the wrapped symbols must stay opaque calls.
    target_compile_options(test_assert PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
    )
    target_link_options(test_assert PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
    )
endif()

add_test(NAME test_assert COMMAND test_assert)

add_executable(test_campaign test_campaign.cpp)
target_link_libraries(test_campaign PRIVATE cosmos Threads::Threads)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Same opaque-call reason as test_wrap_malloc: record_finding and the flush paths allocate
    # through the wrapped heap when a universe is current.
    target_compile_options(test_campaign PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
    )
    target_link_options(test_campaign PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
    )
endif()

add_test(NAME test_campaign COMMAND test_campaign)

add_executable(test_wrap_random test_wrap_random.cpp)
target_link_libraries(test_wrap_random PRIVATE cosmos)
# White-box test: exercises the shared reentrancy guard from wrapper_fault.hpp.
target_include_directories(test_wrap_random PRIVATE ${CMAKE_SOURCE_DIR}/src/cosmos/wrappers)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Wrapped symbols must stay opaque calls; random-family builtins would otherwise fold.
    target_compile_options(test_wrap_random PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
        "-fno-builtin-random"
        "-fno-builtin-rand"
        "-fno-builtin-srand"
        "-fno-builtin-srandom"
    )
    target_link_options(test_wrap_random PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=getrandom"
        "-Wl,--wrap=random"
        "-Wl,--wrap=rand"
        "-Wl,--wrap=srandom"
        "-Wl,--wrap=srand"
    )
endif()

add_test(NAME test_wrap_random COMMAND test_wrap_random)

add_executable(test_wrap_storage test_wrap_storage.cpp)
target_link_libraries(test_wrap_storage PRIVATE cosmos)
# White-box test: pins the internal storage eligibility policy from wrapper_fault.hpp.
target_include_directories(test_wrap_storage PRIVATE ${CMAKE_SOURCE_DIR}/src/cosmos/wrappers)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Wrapped symbols must stay opaque calls so the linker substitution reaches them.
    target_compile_options(test_wrap_storage PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
        "-fno-builtin-open"
        "-fno-builtin-read"
        "-fno-builtin-write"
        "-fno-builtin-fsync"
    )
    target_link_options(test_wrap_storage PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=open"
        "-Wl,--wrap=read"
        "-Wl,--wrap=__read_chk"
        "-Wl,--wrap=write"
        "-Wl,--wrap=fsync"
    )
endif()

add_test(NAME test_wrap_storage COMMAND test_wrap_storage)

add_executable(test_scenario test_scenario.cpp)
target_link_libraries(test_scenario PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # The harness drives real allocations through the wrapped symbols, so they must stay opaque.
    target_compile_options(test_scenario PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
    target_link_options(test_scenario PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=clock_gettime"
    )
endif()

add_test(NAME test_scenario COMMAND test_scenario)

# Second link of the same source. operator new reaches the wrappers only when libstdc++ is linked
# statically, and that is the only configuration in which the harness's own bookkeeping is visible
# to the universe it reports on -- so test_counters_are_snapshotted_at_quiesce can only guard that
# property here. See IMPLEMENTATION_NOTES.md, "what --wrap actually reaches".
#
# Probed rather than assumed: this flake's clang has no static libstdc++, so an unconditional
# target breaks the build-clang and build-asan trees at link time rather than merely skipping.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LINK_OPTIONS "-static-libstdc++")
check_cxx_source_compiles("int main() { return 0; }" COSMOS_HAVE_STATIC_LIBSTDCXX)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

if (COSMOS_HAVE_STATIC_LIBSTDCXX AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(test_scenario_static test_scenario.cpp)
    target_link_libraries(test_scenario_static PRIVATE cosmos)
    target_compile_options(test_scenario_static PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
    target_link_options(test_scenario_static PRIVATE
        "-static-libstdc++"
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=clock_gettime"
    )
    add_test(NAME test_scenario_static COMMAND test_scenario_static)
else()
    message(STATUS "test_scenario_static skipped: no static libstdc++ for this toolchain")
endif()

# The P2-S3 end-to-end fixture: a plain C application that knows nothing about libcosmos, driven by
# the Scenario harness. Compiled twice from one source -- the fix is the -D, so the two targets
# differ only by the nullptr check -- and both run under ctest.
foreach(variant broken fixed)
    set(target test_${variant}_app)
    add_executable(${target}
        test_broken_app.cpp
        ${CMAKE_SOURCE_DIR}/examples/single_node/broken_cache.c
    )
    target_link_libraries(${target} PRIVATE cosmos)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/examples/single_node)
    if(variant STREQUAL fixed)
        target_compile_definitions(${target} PRIVATE BROKEN_CACHE_CHECKS_MALLOC)
    endif()

    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            "-fno-builtin-malloc"
            "-fno-builtin-free"
            "-fno-builtin-calloc"
            "-fno-builtin-realloc"
        )
        target_link_options(${target} PRIVATE
            "-Wl,--gc-sections"
            "-Wl,--wrap=malloc"
            "-Wl,--wrap=free"
            "-Wl,--wrap=calloc"
            "-Wl,--wrap=realloc"
        )
    endif()

    add_test(NAME ${target} COMMAND ${target})
endforeach()

# The leak snapshot is read rather than measured live because a live read self-contaminates
# wherever operator new reaches the wrappers; that only happens under a static libstdc++, so the
# property needs this second link to be guarded rather than argued.
if (COSMOS_HAVE_STATIC_LIBSTDCXX AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(test_broken_app_static
        test_broken_app.cpp
        ${CMAKE_SOURCE_DIR}/examples/single_node/broken_cache.c
    )
    target_link_libraries(test_broken_app_static PRIVATE cosmos)
    target_include_directories(test_broken_app_static
        PRIVATE ${CMAKE_SOURCE_DIR}/examples/single_node)
    target_compile_options(test_broken_app_static PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
    target_link_options(test_broken_app_static PRIVATE
        "-static-libstdc++"
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
    )
    add_test(NAME test_broken_app_static COMMAND test_broken_app_static)
endif()
