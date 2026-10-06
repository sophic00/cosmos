#include "cosmos/cosmos.hpp"
#include "wrapper_fault.hpp"
#include <cerrno>
#include <pthread.h>
#include <sched.h>

// Cooperative fiber interposition (docs/design.md §10). With an active Simulator the whole
// universe runs on one OS thread: create enqueues a fiber, join/mutex/cond/yield suspend the
// current fiber, and the schedule stream picks the next ready fiber. Without one, everything
// passes through — and so must engine work (in_wrapper_logic): std::mutex::lock() is header
// inline, so the allocation registry's mutex reaches these wrappers from inside __wrap_malloc
// itself whenever the pthread family is wrapped. Routing engine sync into the scheduler would
// reschedule a fiber mid-wrapper (mutex_unlock always yields) while every other fiber still
// sees in_wrapper_logic set and silently bypasses allocation tracking. Engine synchronization
// stays on the real OS primitives, which can neither yield a fiber nor deadlock the single OS
// thread.

extern "C" {

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg);
int __real_pthread_join(pthread_t thread, void** retval);
int __real_pthread_detach(pthread_t thread);
pthread_t __real_pthread_self(void);
int __real_pthread_equal(pthread_t t1, pthread_t t2);
void __real_pthread_exit(void* retval);
int __real_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int __real_pthread_mutex_destroy(pthread_mutex_t* mutex);
int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t* mutex);
int __real_pthread_mutex_trylock(pthread_mutex_t* mutex);
int __real_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int __real_pthread_cond_destroy(pthread_cond_t* cond);
int __real_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int __real_pthread_cond_signal(pthread_cond_t* cond);
int __real_pthread_cond_broadcast(pthread_cond_t* cond);
int __real_sched_yield(void);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg) {
    (void)attr;
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_create(thread, attr, start_routine, arg);
    }
    return cosmos::Simulator::current()->scheduler().spawn(thread, start_routine, arg);
}

int __wrap_pthread_join(pthread_t thread, void** retval) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_join(thread, retval);
    }
    return cosmos::Simulator::current()->scheduler().join(thread, retval);
}

int __wrap_pthread_detach(pthread_t thread) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_detach(thread);
    }
    return cosmos::Simulator::current()->scheduler().detach(thread);
}

pthread_t __wrap_pthread_self(void) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_self();
    }
    return cosmos::Simulator::current()->scheduler().self();
}

int __wrap_pthread_equal(pthread_t t1, pthread_t t2) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_equal(t1, t2);
    }
    return t1 == t2 ? 1 : 0;
}

void __wrap_pthread_exit(void* retval) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        __real_pthread_exit(retval);
        return;
    }
    cosmos::Simulator::current()->scheduler().thread_exit(retval);
}

int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_mutex_init(mutex, attr);
    }
    return cosmos::Simulator::current()->scheduler().mutex_init(mutex, attr);
}

int __wrap_pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_mutex_destroy(mutex);
    }
    return cosmos::Simulator::current()->scheduler().mutex_destroy(mutex);
}

int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_mutex_lock(mutex);
    }
    return cosmos::Simulator::current()->scheduler().mutex_lock(mutex);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_mutex_unlock(mutex);
    }
    return cosmos::Simulator::current()->scheduler().mutex_unlock(mutex);
}

int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_mutex_trylock(mutex);
    }
    return cosmos::Simulator::current()->scheduler().mutex_trylock(mutex);
}

int __wrap_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_cond_init(cond, attr);
    }
    return cosmos::Simulator::current()->scheduler().cond_init(cond, attr);
}

int __wrap_pthread_cond_destroy(pthread_cond_t* cond) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_cond_destroy(cond);
    }
    return cosmos::Simulator::current()->scheduler().cond_destroy(cond);
}

int __wrap_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_cond_wait(cond, mutex);
    }
    return cosmos::Simulator::current()->scheduler().cond_wait(cond, mutex);
}

int __wrap_pthread_cond_signal(pthread_cond_t* cond) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_cond_signal(cond);
    }
    return cosmos::Simulator::current()->scheduler().cond_signal(cond);
}

int __wrap_pthread_cond_broadcast(pthread_cond_t* cond) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pthread_cond_broadcast(cond);
    }
    return cosmos::Simulator::current()->scheduler().cond_broadcast(cond);
}

int __wrap_sched_yield(void) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_sched_yield();
    }
    cosmos::Simulator::current()->scheduler().yield();
    return 0;
}

} // extern "C"
