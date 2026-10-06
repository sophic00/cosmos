#include "cosmos/task.hpp"

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <time.h>
#include <unistd.h>

extern "C" {
void* __real_malloc(size_t size);
void __real_free(void* ptr);
}

namespace cosmos {
namespace {

bool timespec_valid(const struct timespec& ts) {
    return ts.tv_sec >= 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1'000'000'000L;
}

int64_t timespec_ns(const struct timespec& ts) {
    return add_sat(mul_sat(static_cast<int64_t>(ts.tv_sec), 1'000'000'000LL),
                   static_cast<int64_t>(ts.tv_nsec));
}

void write_zero(struct timespec* rem) {
    if (rem != nullptr) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
}

// Single OS thread, so a handoff set before each swap is always consumed by
// the fiber we switch to before any other switch can overwrite it.
thread_local Scheduler* t_active_scheduler = nullptr;

bool timer_holds(const std::vector<Timer, RawRealAllocator<Timer>>& timers, Task* self) {
    for (const Timer& timer : timers) {
        if (timer.task == self) return true;
    }
    return false;
}

void sleep_block(Scheduler* sched);

} // namespace

Scheduler::Scheduler(uint64_t schedule_seed, VirtualClock& clock)
    : schedule_rng_(schedule_seed), clock_(clock) {
    main_task_.id = 0;
    main_task_.state = Task::State::Running;
    main_task_.owner = this;
    current_ = &main_task_;
}

Scheduler::~Scheduler() {
    for (Task* task : all_tasks_) {
        if (task == nullptr) continue;
        task->~Task();
        if (task->stack != nullptr) __real_free(task->stack);
        __real_free(task);
    }
}

Task* Scheduler::find_task(pthread_t id) {
    for (Task* task : all_tasks_) {
        if (task != nullptr && task->id == id) return task;
    }
    return nullptr;
}

SimMutex* Scheduler::find_mutex(pthread_mutex_t* key) {
    for (auto& mu : mutexes_) {
        if (mu.key == key) return &mu;
    }
    return nullptr;
}

SimMutex& Scheduler::get_or_create_mutex(pthread_mutex_t* key) {
    if (SimMutex* found = find_mutex(key)) return *found;
    SimMutex mu;
    mu.key = key;
    mutexes_.push_back(mu);
    return mutexes_.back();
}

SimCond* Scheduler::find_cond(pthread_cond_t* key) {
    for (auto& cond : conds_) {
        if (cond.key == key) return &cond;
    }
    return nullptr;
}

SimCond& Scheduler::get_or_create_cond(pthread_cond_t* key) {
    if (SimCond* found = find_cond(key)) return *found;
    SimCond cond;
    cond.key = key;
    conds_.push_back(cond);
    return conds_.back();
}

Task* Scheduler::pick_next_ready() {
    if (ready_.size() == 1) {
        Task* task = ready_.back();
        ready_.pop_back();
        return task;
    }
    const uint64_t idx = schedule_rng_.range(0, ready_.size() - 1);
    Task* task = ready_[static_cast<size_t>(idx)];
    ready_.erase(ready_.begin() + static_cast<ptrdiff_t>(idx));
    return task;
}

void Scheduler::switch_to(Task* next) {
    Task* prev = current_;
    if (prev == next) {
        next->state = Task::State::Running;
        return;
    }
    current_ = next;
    next->state = Task::State::Running;
    t_active_scheduler = this;
    swapcontext(&prev->ctx, &next->ctx);
}

bool Scheduler::has_blocked() const {
    for (const Task* task : all_tasks_) {
        if (task == nullptr) continue;
        switch (task->state) {
        case Task::State::BlockedMutex:
        case Task::State::BlockedCond:
        case Task::State::BlockedJoin:
        case Task::State::Sleeping:
            return true;
        default:
            break;
        }
    }
    return false;
}

void Scheduler::push_timer(Task* task, Time wakeup) {
    timers_.push_back(Timer{wakeup, timer_seq_++, task});
}

bool Scheduler::advance_to_next_timer() {
    if (timers_.empty()) return false;
    Time earliest = timers_.front().wakeup;
    for (const Timer& timer : timers_) {
        if (timer.wakeup < earliest) earliest = timer.wakeup;
    }
    clock_.advance_to(earliest);
    std::vector<Timer, RawRealAllocator<Timer>> remaining;
    remaining.reserve(timers_.size());
    for (const Timer& timer : timers_) {
        if (timer.wakeup <= earliest) {
            timer.task->state = Task::State::Ready;
            timer.task->wake_error = 0;
            ready_.push_back(timer.task);
        } else {
            remaining.push_back(timer);
        }
    }
    timers_.swap(remaining);
    return true;
}

[[noreturn]] void Scheduler::abort_deadlock(const char* what) {
    const char prefix[] = "cosmos: deadlock without drain in ";
    (void)::write(2, prefix, sizeof(prefix) - 1);
    size_t len = 0;
    while (what[len] != '\0')
        ++len;
    (void)::write(2, what, len);
    (void)::write(2, "\n", 1);
    ::abort();
}

int Scheduler::spawn(pthread_t* out, void* (*start)(void*), void* arg) {
    if (out == nullptr || start == nullptr) return EINVAL;
    void* task_mem = __real_malloc(sizeof(Task));
    if (task_mem == nullptr) return EAGAIN;
    void* stack = __real_malloc(kFiberStackSize);
    if (stack == nullptr) {
        __real_free(task_mem);
        return EAGAIN;
    }
    auto* task = new (task_mem) Task();
    task->stack = stack;
    task->stack_size = kFiberStackSize;
    task->func = start;
    task->arg = arg;
    do {
        task->id = static_cast<pthread_t>(next_id_++);
    } while (task->id == 0);
    task->state = Task::State::Ready;
    task->owner = this;
    if (::getcontext(&task->ctx) != 0) {
        task->~Task();
        __real_free(stack);
        __real_free(task_mem);
        return EAGAIN;
    }
    task->ctx.uc_stack.ss_sp = stack;
    task->ctx.uc_stack.ss_size = kFiberStackSize;
    task->ctx.uc_stack.ss_flags = 0;
    task->ctx.uc_link = nullptr;
    ::makecontext(&task->ctx, reinterpret_cast<void (*)()>(&Scheduler::task_trampoline), 0);
    ready_.push_back(task);
    all_tasks_.push_back(task);
    *out = task->id;
    return 0;
}

int Scheduler::join(pthread_t id, void** retval) {
    Task* target = find_task(id);
    if (target == nullptr) return ESRCH;
    if (target->id == current_->id) return EDEADLK;
    if (target->detached) return EINVAL;
    if (target->state == Task::State::Done) {
        if (retval != nullptr) *retval = target->retval;
        return 0;
    }
    Task* self = current_;
    target->joiners.push_back(self);
    self->state = Task::State::BlockedJoin;
    self->wake_error = 0;
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else if (!timers_.empty()) {
        advance_to_next_timer();
        switch_to(pick_next_ready());
    } else {
        if (drain_requester_ != nullptr) {
            switch_to(drain_requester_);
        } else {
            auto& joiners = target->joiners;
            for (auto it = joiners.begin(); it != joiners.end(); ++it) {
                if (*it == self) {
                    joiners.erase(it);
                    break;
                }
            }
            self->state = Task::State::Running;
            abort_deadlock("pthread_join");
        }
    }
    if (self->wake_error != 0) return self->wake_error;
    if (retval != nullptr) *retval = target->retval;
    return 0;
}

int Scheduler::detach(pthread_t id) {
    Task* target = find_task(id);
    if (target == nullptr) return ESRCH;
    if (target->detached) return EINVAL;
    target->detached = true;
    return 0;
}

pthread_t Scheduler::self() { return current_->id; }

[[noreturn]] void Scheduler::thread_exit(void* retval) {
    Task* self = current_;
    if (self == &main_task_) {
        main_task_.retval = retval;
        main_task_.state = Task::State::Done;
        while (!ready_.empty() || !timers_.empty()) {
            if (ready_.empty()) {
                advance_to_next_timer();
                continue;
            }
            switch_to(pick_next_ready());
        }
        ::exit(has_blocked() ? 1 : 0);
    }
    self->retval = retval;
    on_task_exit(self);
    ::abort();
}

int Scheduler::mutex_init(pthread_mutex_t* mu, const pthread_mutexattr_t* attr) {
    (void)attr;
    if (mu == nullptr) return EINVAL;
    SimMutex* existing = find_mutex(mu);
    if (existing != nullptr) {
        if (existing->locked || !existing->waiters.empty()) return EBUSY;
        existing->locked = false;
        existing->owner = nullptr;
        return 0;
    }
    SimMutex entry;
    entry.key = mu;
    mutexes_.push_back(entry);
    return 0;
}

int Scheduler::mutex_destroy(pthread_mutex_t* mu) {
    if (mu == nullptr) return EINVAL;
    for (auto it = mutexes_.begin(); it != mutexes_.end(); ++it) {
        if (it->key != mu) continue;
        if (it->locked || !it->waiters.empty()) return EBUSY;
        mutexes_.erase(it);
        return 0;
    }
    return 0;
}

int Scheduler::mutex_lock(pthread_mutex_t* mu) {
    if (mu == nullptr) return EINVAL;
    SimMutex& entry = get_or_create_mutex(mu);
    Task* self = current_;
    if (!entry.locked) {
        entry.locked = true;
        entry.owner = self;
        return 0;
    }
    if (entry.owner == self) return EDEADLK;
    entry.waiters.push_back(self);
    self->state = Task::State::BlockedMutex;
    self->wake_error = 0;
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else if (!timers_.empty()) {
        advance_to_next_timer();
        switch_to(pick_next_ready());
    } else {
        if (drain_requester_ != nullptr) {
            switch_to(drain_requester_);
        } else {
            auto& waiters = entry.waiters;
            for (auto it = waiters.begin(); it != waiters.end(); ++it) {
                if (*it == self) {
                    waiters.erase(it);
                    break;
                }
            }
            self->state = Task::State::Running;
            abort_deadlock("pthread_mutex_lock");
        }
    }
    if (self->wake_error != 0) return self->wake_error;
    return 0;
}

int Scheduler::mutex_unlock(pthread_mutex_t* mu) {
    if (mu == nullptr) return EINVAL;
    SimMutex* entry = find_mutex(mu);
    if (entry == nullptr || !entry->locked || entry->owner != current_) return EPERM;
    if (!entry->waiters.empty()) {
        Task* waiter = entry->waiters.front();
        entry->waiters.erase(entry->waiters.begin());
        entry->owner = waiter;
        waiter->state = Task::State::Ready;
        waiter->wake_error = 0;
        ready_.push_back(waiter);
    } else {
        entry->locked = false;
        entry->owner = nullptr;
    }
    Task* self = current_;
    self->state = Task::State::Ready;
    self->wake_error = 0;
    ready_.push_back(self);
    switch_to(pick_next_ready());
    return 0;
}

int Scheduler::mutex_trylock(pthread_mutex_t* mu) {
    if (mu == nullptr) return EINVAL;
    SimMutex& entry = get_or_create_mutex(mu);
    if (!entry.locked) {
        entry.locked = true;
        entry.owner = current_;
        return 0;
    }
    return EBUSY;
}

int Scheduler::cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    (void)attr;
    if (cond == nullptr) return EINVAL;
    if (find_cond(cond) != nullptr) return EBUSY;
    SimCond entry;
    entry.key = cond;
    conds_.push_back(entry);
    return 0;
}

int Scheduler::cond_destroy(pthread_cond_t* cond) {
    if (cond == nullptr) return EINVAL;
    for (auto it = conds_.begin(); it != conds_.end(); ++it) {
        if (it->key != cond) continue;
        if (!it->waiters.empty()) return EBUSY;
        conds_.erase(it);
        return 0;
    }
    return 0;
}

int Scheduler::cond_wait(pthread_cond_t* cond, pthread_mutex_t* mu) {
    if (cond == nullptr || mu == nullptr) return EINVAL;
    SimCond& cond_entry = get_or_create_cond(cond);
    SimMutex& mu_entry = get_or_create_mutex(mu);
    Task* self = current_;
    if (!mu_entry.locked || mu_entry.owner != self) return EPERM;
    if (!mu_entry.waiters.empty()) {
        Task* waiter = mu_entry.waiters.front();
        mu_entry.waiters.erase(mu_entry.waiters.begin());
        mu_entry.owner = waiter;
        waiter->state = Task::State::Ready;
        waiter->wake_error = 0;
        ready_.push_back(waiter);
    } else {
        mu_entry.locked = false;
        mu_entry.owner = nullptr;
    }
    cond_entry.waiters.push_back(SimCond::Waiter{self, mu});
    self->state = Task::State::BlockedCond;
    self->wake_error = 0;
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else if (!timers_.empty()) {
        advance_to_next_timer();
        switch_to(pick_next_ready());
    } else {
        if (drain_requester_ != nullptr) {
            switch_to(drain_requester_);
        } else {
            auto& waiters = cond_entry.waiters;
            for (auto it = waiters.begin(); it != waiters.end(); ++it) {
                if (it->task == self) {
                    waiters.erase(it);
                    break;
                }
            }
            self->state = Task::State::Running;
            abort_deadlock("pthread_cond_wait");
        }
    }
    if (self->wake_error != 0) return self->wake_error;
    return 0;
}

void Scheduler::deliver_cond_waiter(SimCond::Waiter waiter) {
    SimMutex& mu_entry = get_or_create_mutex(waiter.mu);
    Task* task = waiter.task;
    if (!mu_entry.locked) {
        mu_entry.locked = true;
        mu_entry.owner = task;
        task->state = Task::State::Ready;
        task->wake_error = 0;
        ready_.push_back(task);
    } else {
        mu_entry.waiters.push_back(task);
        task->state = Task::State::BlockedMutex;
        task->wake_error = 0;
    }
}

int Scheduler::cond_signal(pthread_cond_t* cond) {
    if (cond == nullptr) return EINVAL;
    if (SimCond* entry = find_cond(cond); entry != nullptr && !entry->waiters.empty()) {
        SimCond::Waiter waiter = entry->waiters.front();
        entry->waiters.erase(entry->waiters.begin());
        deliver_cond_waiter(waiter);
    }
    Task* self = current_;
    self->state = Task::State::Ready;
    self->wake_error = 0;
    ready_.push_back(self);
    switch_to(pick_next_ready());
    return 0;
}

int Scheduler::cond_broadcast(pthread_cond_t* cond) {
    if (cond == nullptr) return EINVAL;
    if (SimCond* entry = find_cond(cond); entry != nullptr && !entry->waiters.empty()) {
        std::vector<SimCond::Waiter, RawRealAllocator<SimCond::Waiter>> waking;
        waking.swap(entry->waiters);
        for (SimCond::Waiter waiter : waking)
            deliver_cond_waiter(waiter);
    }
    Task* self = current_;
    self->state = Task::State::Ready;
    self->wake_error = 0;
    ready_.push_back(self);
    switch_to(pick_next_ready());
    return 0;
}

void Scheduler::yield() {
    Task* self = current_;
    self->state = Task::State::Ready;
    self->wake_error = 0;
    ready_.push_back(self);
    switch_to(pick_next_ready());
}

int Scheduler::sleep_ns(const struct timespec* req, struct timespec* rem) {
    if (req == nullptr) {
        errno = EFAULT;
        return -1;
    }
    if (!timespec_valid(*req)) {
        errno = EINVAL;
        return -1;
    }
    const int64_t ns = timespec_ns(*req);
    if (ns <= 0) {
        write_zero(rem);
        return 0;
    }
    Task* self = current_;
    push_timer(self, clock_.now() + Duration{ns});
    self->state = Task::State::Sleeping;
    self->wake_error = 0;
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else {
        advance_to_next_timer();
        if (timer_holds(timers_, self)) {
            switch_to(pick_next_ready());
        } else {
            for (auto it = ready_.begin(); it != ready_.end(); ++it) {
                if (*it == self) {
                    ready_.erase(it);
                    break;
                }
            }
            self->state = Task::State::Running;
        }
    }
    write_zero(rem);
    return 0;
}

int Scheduler::sleep_clock(clockid_t clk, int flags, const struct timespec* req,
                           struct timespec* rem) {
    if (req == nullptr) return EFAULT;
    if (!timespec_valid(*req)) return EINVAL;
    if (flags != 0 && flags != TIMER_ABSTIME) return EINVAL;
    bool realtime = false;
    switch (clk) {
    case CLOCK_MONOTONIC:
#ifdef CLOCK_MONOTONIC_RAW
    case CLOCK_MONOTONIC_RAW:
#endif
#ifdef CLOCK_BOOTTIME
    case CLOCK_BOOTTIME:
#endif
#ifdef CLOCK_MONOTONIC_COARSE
    case CLOCK_MONOTONIC_COARSE:
#endif
        realtime = false;
        break;
    case CLOCK_REALTIME:
#ifdef CLOCK_REALTIME_COARSE
    case CLOCK_REALTIME_COARSE:
#endif
        realtime = true;
        break;
    default:
        return EINVAL;
    }
    if (flags == TIMER_ABSTIME) {
        // clock_nanosleep(2): the remainder is unused for an absolute sleep, so leave it untouched.
        const int64_t deadline_ns = timespec_ns(*req);
        const Time target =
            realtime ? Time{sub_sat(deadline_ns, clock_.realtime_epoch_ns())} : Time{deadline_ns};
        if (target <= clock_.now()) {
            return 0;
        }
        Task* self = current_;
        push_timer(self, target);
        self->state = Task::State::Sleeping;
        self->wake_error = 0;
        if (!ready_.empty()) {
            switch_to(pick_next_ready());
        } else {
            advance_to_next_timer();
            if (timer_holds(timers_, self)) {
                switch_to(pick_next_ready());
            } else {
                for (auto it = ready_.begin(); it != ready_.end(); ++it) {
                    if (*it == self) {
                        ready_.erase(it);
                        break;
                    }
                }
                self->state = Task::State::Running;
            }
        }
        return 0;
    }
    const int64_t ns = timespec_ns(*req);
    if (ns <= 0) {
        write_zero(rem);
        return 0;
    }
    Task* self = current_;
    push_timer(self, clock_.now() + Duration{ns});
    self->state = Task::State::Sleeping;
    self->wake_error = 0;
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else {
        advance_to_next_timer();
        if (timer_holds(timers_, self)) {
            switch_to(pick_next_ready());
        } else {
            for (auto it = ready_.begin(); it != ready_.end(); ++it) {
                if (*it == self) {
                    ready_.erase(it);
                    break;
                }
            }
            self->state = Task::State::Running;
        }
    }
    write_zero(rem);
    return 0;
}

void Scheduler::run_until_quiescence() {
    if (drain_requester_ != nullptr) return;
    drain_requester_ = current_;
    deadlocked_ = false;
    while (true) {
        if (ready_.empty() && timers_.empty()) break;
        if (ready_.empty()) {
            advance_to_next_timer();
            continue;
        }
        switch_to(pick_next_ready());
    }
    deadlocked_ = has_blocked();
    drain_requester_ = nullptr;
}

void Scheduler::wake_joiners(Task* done) {
    for (Task* joiner : done->joiners) {
        joiner->state = Task::State::Ready;
        joiner->wake_error = 0;
        ready_.push_back(joiner);
    }
    done->joiners.clear();
}

void Scheduler::task_trampoline() {
    Scheduler* sched = t_active_scheduler;
    // The switcher set current_ before swapping in, so this is us.
    Task* self = sched->current_;
    void* retval = self->func(self->arg);
    self->retval = retval;
    sched->on_task_exit(self);
    ::abort();
}

void Scheduler::on_task_exit(Task* done) {
    done->state = Task::State::Done;
    wake_joiners(done);
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else if (!timers_.empty()) {
        advance_to_next_timer();
        switch_to(pick_next_ready());
    } else {
        if (drain_requester_ != nullptr) {
            deadlocked_ = has_blocked();
            switch_to(drain_requester_);
        } else if (main_task_.state == Task::State::Done) {
            ::exit(has_blocked() ? 1 : 0);
        } else if (has_blocked()) {
            abort_deadlock("fiber exit");
        } else {
            abort_deadlock("fiber exit (quiescent without drain)");
        }
    }
    ::abort();
}

} // namespace cosmos
