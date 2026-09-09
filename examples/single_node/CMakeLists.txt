# Single binary application: Transactional WAL Key-Value Storage Engine
# Tests crash durability (fsync, torn writes) and heap OOM fault injection

# Layman cache demo: one source, two binaries for the 3-step story.
# prod runs natively (no faults possible), sim runs through libcosmos OOM
# injection with --seed selecting the fault beforehand (seed % 22).
add_executable(cache_demo_prod cache_demo.cpp broken_cache.c)
target_include_directories(cache_demo_prod PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cache_demo_prod PRIVATE COSMOS_PROD BROKEN_CACHE_CHECKS_MALLOC)

add_executable(cache_demo_sim cache_demo.cpp broken_cache.c)
target_include_directories(cache_demo_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cache_demo_sim PRIVATE COSMOS_SIM BROKEN_CACHE_CHECKS_MALLOC)
target_link_libraries(cache_demo_sim PRIVATE cosmos)

add_executable(kv_store_prod kv_store.c)
target_compile_definitions(kv_store_prod PRIVATE COSMOS_PROD)

add_executable(kv_store_sim kv_store.c)
target_compile_definitions(kv_store_sim PRIVATE COSMOS_SIM)
target_link_libraries(kv_store_sim PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  # The demo drives real allocations through the wrapped symbols, so they must stay opaque.
  target_compile_options(cache_demo_sim PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
  target_link_options(cache_demo_sim PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
    )
  target_compile_options(kv_store_sim PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
        "-fno-builtin-random"
        "-fno-builtin-rand"
        "-fno-builtin-srand"
        "-fno-builtin-srandom"
    )
  target_link_options(kv_store_sim PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=pthread_create"
        "-Wl,--wrap=pthread_join"
        "-Wl,--wrap=pthread_mutex_lock"
        "-Wl,--wrap=pthread_mutex_unlock"
        "-Wl,--wrap=pthread_cond_wait"
        "-Wl,--wrap=pthread_cond_signal"
        "-Wl,--wrap=sched_yield"
        "-Wl,--wrap=clock_gettime"
        "-Wl,--wrap=nanosleep"
        "-Wl,--wrap=clock_nanosleep"
        "-Wl,--wrap=open"
        "-Wl,--wrap=read"
        "-Wl,--wrap=__read_chk"
        "-Wl,--wrap=write"
        "-Wl,--wrap=fsync"
        "-Wl,--wrap=getrandom"
        "-Wl,--wrap=random"
        "-Wl,--wrap=rand"
        "-Wl,--wrap=srandom"
        "-Wl,--wrap=srand"
    )
endif()
