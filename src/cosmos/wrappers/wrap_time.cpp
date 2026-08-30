#include "cosmos/cosmos.hpp"
#include <sys/time.h>
#include <time.h>

// Linker-wrapping passthrough stubs for the POSIX time surface
// (see docs/design.md §3 "Time"). Virtual deterministic time is layered on
// later; for now they return the real host clock via __real_*.

extern "C" {

int __real_clock_gettime(clockid_t clock_id, struct timespec* tp);
int __real_gettimeofday(struct timeval* tv, void* tz);
int __real_nanosleep(const struct timespec* req, struct timespec* rem);

int __wrap_clock_gettime(clockid_t clock_id, struct timespec* tp) {
    return __real_clock_gettime(clock_id, tp);
}

int __wrap_gettimeofday(struct timeval* tv, void* tz) { return __real_gettimeofday(tv, tz); }

int __wrap_nanosleep(const struct timespec* req, struct timespec* rem) {
    return __real_nanosleep(req, rem);
}

} // extern "C"
