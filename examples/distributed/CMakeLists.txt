# Distributed application: Replicated Primary-Backup Consensus Node Cluster

add_executable(replicated_kv_prod replicated_kv.c)
target_compile_definitions(replicated_kv_prod PRIVATE COSMOS_PROD)

add_executable(replicated_kv_sim replicated_kv.c)
target_compile_definitions(replicated_kv_sim PRIVATE COSMOS_SIM)
target_link_libraries(replicated_kv_sim PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(replicated_kv_sim PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
  target_link_options(replicated_kv_sim PRIVATE
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
        "-Wl,--wrap=socket"
        "-Wl,--wrap=bind"
        "-Wl,--wrap=listen"
        "-Wl,--wrap=accept"
        "-Wl,--wrap=connect"
        "-Wl,--wrap=send"
        "-Wl,--wrap=recv"
        "-Wl,--wrap=close"
        "-Wl,--wrap=getrandom"
    )
endif()
