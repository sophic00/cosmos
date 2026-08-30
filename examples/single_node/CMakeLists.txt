# Single binary application: Transactional WAL Key-Value Storage Engine
# Tests crash durability (fsync, torn writes) and heap OOM fault injection

add_executable(kv_store_prod kv_store.c)
target_compile_definitions(kv_store_prod PRIVATE COSMOS_PROD)

add_executable(kv_store_sim kv_store.c)
target_compile_definitions(kv_store_sim PRIVATE COSMOS_SIM)
target_link_libraries(kv_store_sim PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(kv_store_sim PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
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
        "-Wl,--wrap=open"
        "-Wl,--wrap=read"
        "-Wl,--wrap=write"
        "-Wl,--wrap=fsync"
        "-Wl,--wrap=getrandom"
    )
endif()
