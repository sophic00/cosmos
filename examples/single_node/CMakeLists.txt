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
        "-Wl,--wrap=pthread_detach"
        "-Wl,--wrap=pthread_self"
        "-Wl,--wrap=pthread_equal"
        "-Wl,--wrap=pthread_exit"
        "-Wl,--wrap=pthread_mutex_init"
        "-Wl,--wrap=pthread_mutex_destroy"
        "-Wl,--wrap=pthread_mutex_lock"
        "-Wl,--wrap=pthread_mutex_unlock"
        "-Wl,--wrap=pthread_mutex_trylock"
        "-Wl,--wrap=pthread_cond_init"
        "-Wl,--wrap=pthread_cond_destroy"
        "-Wl,--wrap=pthread_cond_wait"
        "-Wl,--wrap=pthread_cond_signal"
        "-Wl,--wrap=pthread_cond_broadcast"
        "-Wl,--wrap=sched_yield"
        "-Wl,--wrap=clock_gettime"
        "-Wl,--wrap=gettimeofday"
        "-Wl,--wrap=nanosleep"
        "-Wl,--wrap=clock_nanosleep"
        "-Wl,--wrap=open"
        "-Wl,--wrap=open64"
        "-Wl,--wrap=__open_2"
        "-Wl,--wrap=__open64_2"
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
