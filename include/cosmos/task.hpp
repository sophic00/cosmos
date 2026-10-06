#pragma once

// Deterministic single-threaded fiber scheduler for wrapped pthreads
// (docs/design.md §10). No OS threads are created: pthread_create enqueues a
// fiber, mutex/cond/join/sleep suspend the current fiber, and every choice of
// which ready fiber runs next draws from the Schedule stream. Virtual time
// advances only when no fiber is runnable, to the earliest timer.

#include "cosmos/memory.hpp"
#include "cosmos/random.hpp"
#include "cosmos/time.hpp"

#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <ucontext.h>
#include <vector>

namespace cosmos {

// 256 KiB per fiber: enough for test workloads doing printf/malloc, small
// enough for dozens of tasks. Allocated via __real_malloc so engine stacks
// never enter the tracked heap or the fault injector (Rule 7).
inline constexpr size_t kFiberStackSize = 256 * 1024;

struct Task {
    enum class State : uint8_t {
        Ready,
        Running,
        BlockedMutex,
        BlockedCond,
        BlockedJoin,
        Sleeping,
        Done,
    };

    ucontext_t ctx{};
    void* stack = nullptr;
    size_t stack_size = 0;
    void* (*func)(void*) = nullptr;
    void* arg = nullptr;
    void* retval = nullptr;
    pthread_t id = 0;
    State state = State::Ready;
    bool detached = false;
    // Error to return when resumed via deadlock fallback. 0 means normal wake.
    int wake_error = 0;
    class Scheduler* owner = nullptr;
    // Tasks waiting to join this one. Moved to ready on exit.
    std::vector<Task*, RawRealAllocator<Task*>> joiners;
};

struct SimMutex {
    pthread_mutex_t* key = nullptr;
    bool locked = false;
    Task* owner = nullptr;
    std::vector<Task*, RawRealAllocator<Task*>> waiters;
};

struct SimCond {
    struct Waiter {
        Task* task = nullptr;
        pthread_mutex_t* mu = nullptr;
    };
    pthread_cond_t* key = nullptr;
    std::vector<Waiter, RawRealAllocator<Waiter>> waiters;
};

struct Timer {
    Time wakeup{};
    uint64_t seq = 0;
    Task* task = nullptr;
};

class Scheduler {
  public:
    explicit Scheduler(uint64_t schedule_seed, VirtualClock& clock);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Spawning. Returns 0 or an errno-style error (EINVAL/EAGAIN). Never
    // switches: the child runs when the current fiber next blocks or drains.
    int spawn(pthread_t* out, void* (*start)(void*), void* arg);

    // Joining. Blocks the caller until target completes unless target is
    // already done. Returns 0, ESRCH, EINVAL (detached), or EDEADLK (self).
    int join(pthread_t id, void** retval);

    int detach(pthread_t id);
    pthread_t self();
    // Terminates the calling fiber. Never returns for workers. Main calling
    // exit drains remaining fibers and terminates the process.
    [[noreturn]] void thread_exit(void* retval);

    int mutex_init(pthread_mutex_t* mu, const pthread_mutexattr_t* attr);
    int mutex_destroy(pthread_mutex_t* mu);
    int mutex_lock(pthread_mutex_t* mu);
    int mutex_unlock(pthread_mutex_t* mu);
    int mutex_trylock(pthread_mutex_t* mu);

    int cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
    int cond_destroy(pthread_cond_t* cond);
    int cond_wait(pthread_cond_t* cond, pthread_mutex_t* mu);
    int cond_signal(pthread_cond_t* cond);
    int cond_broadcast(pthread_cond_t* cond);

    void yield();

    // Blocking sleeps. Validation mirrors VirtualClock so wrappers stay thin.
    // nanosleep convention: 0 or -1 with errno. clock_nanosleep convention:
    // error number directly.
    int sleep_ns(const struct timespec* req, struct timespec* rem);
    int sleep_clock(clockid_t clk, int flags, const struct timespec* req, struct timespec* rem);

    // Drains all fibers. Called with the main fiber running. Returns with the
    // main fiber running; check deadlocked() for ABBA-style hangs.
    void run_until_quiescence();

    bool deadlocked() const { return deadlocked_; }
    Task* current_task() { return current_; }
    const Task* current_task() const { return current_; }
    Rng& schedule_rng() { return schedule_rng_; }
    size_t ready_count() const { return ready_.size(); }
    size_t task_count() const { return all_tasks_.size(); }

    // Trampoline + exit path, public for makecontext.
    static void task_trampoline();
    void on_task_exit(Task* done);

  private:
    Task* find_task(pthread_t id);
    SimMutex* find_mutex(pthread_mutex_t* key);
    SimMutex& get_or_create_mutex(pthread_mutex_t* key);
    SimCond* find_cond(pthread_cond_t* key);
    SimCond& get_or_create_cond(pthread_cond_t* key);

    // Pops one ready fiber. Draws from the schedule stream only when the
    // choice matters (size > 1) so single-fiber runs never consume draws.
    Task* pick_next_ready();
    void switch_to(Task* next);
    void wake_joiners(Task* done);
    // Moves one cond waiter to runnable, reacquiring its mutex when free and
    // parking on that mutex queue otherwise. Callers pop FIFO for determinism.
    void deliver_cond_waiter(SimCond::Waiter waiter);
    bool has_blocked() const;
    void push_timer(Task* task, Time wakeup);
    // Advances to the earliest timer and wakes everything due at that stamp.
    // Returns false when no timers exist.
    bool advance_to_next_timer();
    [[noreturn]] void abort_deadlock(const char* what);

    Rng schedule_rng_;
    VirtualClock& clock_;
    Task main_task_;
    Task* current_ = nullptr;
    std::vector<Task*, RawRealAllocator<Task*>> ready_;
    std::vector<Task*, RawRealAllocator<Task*>> all_tasks_;
    std::vector<SimMutex, RawRealAllocator<SimMutex>> mutexes_;
    std::vector<SimCond, RawRealAllocator<SimCond>> conds_;
    std::vector<Timer, RawRealAllocator<Timer>> timers_;
    uint64_t timer_seq_ = 0;
    uint64_t next_id_ = 1;
    bool deadlocked_ = false;
    Task* drain_requester_ = nullptr;
};

} // namespace cosmos
