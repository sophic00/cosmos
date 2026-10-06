#include "cosmos/cosmos.hpp"

#include "wrapper_fault.hpp"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/random.h>
#include <sys/types.h>

namespace {

unsigned allowed_getrandom_flags() {
    unsigned allowed = 0;
#ifdef GRND_NONBLOCK
    allowed |= static_cast<unsigned>(GRND_NONBLOCK);
#endif
#ifdef GRND_RANDOM
    allowed |= static_cast<unsigned>(GRND_RANDOM);
#endif
#ifdef GRND_INSECURE
    allowed |= static_cast<unsigned>(GRND_INSECURE);
#endif
    return allowed;
}

} // namespace

extern "C" {

ssize_t __real_getrandom(void* buf, size_t buflen, unsigned int flags);
long int __real_random(void);
int __real_rand(void);
void __real_srandom(unsigned int seed);
void __real_srand(unsigned int seed);

ssize_t __wrap_getrandom(void* buf, size_t buflen, unsigned int flags) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_getrandom(buf, buflen, flags);
    }

    // Validation order mirrors the kernel's (drivers/char/random.c): flags, then count, then the
    // buffer. Reordering changes which errno a bad call reports.
    if ((flags & ~allowed_getrandom_flags()) != 0) {
        errno = EINVAL;
        return -1;
    }
#if defined(GRND_INSECURE) && defined(GRND_RANDOM)
    if ((flags & static_cast<unsigned>(GRND_INSECURE)) != 0 &&
        (flags & static_cast<unsigned>(GRND_RANDOM)) != 0) {
        errno = EINVAL;
        return -1;
    }
#endif

    // Clamped like the kernel, or the ssize_t cast below reports success as a negative error.
    const size_t count =
        buflen > static_cast<size_t>(INT_MAX) ? static_cast<size_t>(INT_MAX) : buflen;
    if (count == 0) {
        return 0;
    }
    if (buf == nullptr) {
        errno = EFAULT;
        return -1;
    }

    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    // Decision first, values second: a failed call must not consume the User stream (Rule 1).
    if (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Random, cosmos::SiteId::getrandom) ==
        cosmos::FaultKind::RandomEagain) {
        errno = EAGAIN;
        return -1;
    }

    // Allocation-free (Rule 7): a stack word and memcpy, never a buffer.
    auto* out = static_cast<unsigned char*>(buf);
    size_t remaining = count;
    while (remaining > 0) {
        const uint64_t word = sim->user_rng().next();
        const size_t chunk = remaining < sizeof(word) ? remaining : sizeof(word);
        memcpy(out, &word, chunk);
        out += chunk;
        remaining -= chunk;
    }
    return static_cast<ssize_t>(count);
}

long int __wrap_random(void) {
    if (!cosmos::Simulator::has_current()) {
        return __real_random();
    }
    // No SiteId::random and no legal FaultKind: value-only, never draws from a fault stream.
    auto* sim = cosmos::Simulator::current();
    return static_cast<long int>(sim->user_rng().range(0, static_cast<uint64_t>(RAND_MAX)));
}

// rand() does not re-enter our wrapped random(), so it needs its own wrapper or it leaks host
// nondeterminism. Shares the User stream with random(), consumed in call order.
int __wrap_rand(void) {
    if (!cosmos::Simulator::has_current()) {
        return __real_rand();
    }
    auto* sim = cosmos::Simulator::current();
    return static_cast<int>(sim->user_rng().range(0, static_cast<uint64_t>(RAND_MAX)));
}

// Without a universe this is host code seeding the host RNG, and it reaches the real call like
// every other passthrough wrapper — a sim binary's non-simulation phases must not behave
// differently from the prod binary. With a universe it stays a no-op: host seeding must not
// perturb the User stream, and universe values must not depend on app seeding order.
void __wrap_srandom(unsigned int seed) {
    if (!cosmos::Simulator::has_current()) {
        __real_srandom(seed);
        return;
    }
    (void)seed;
}

void __wrap_srand(unsigned int seed) {
    if (!cosmos::Simulator::has_current()) {
        __real_srand(seed);
        return;
    }
    (void)seed;
}

} // extern "C"
