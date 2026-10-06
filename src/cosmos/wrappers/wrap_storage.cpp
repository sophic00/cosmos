#include "cosmos/cosmos.hpp"

#include "wrapper_fault.hpp"

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Storage translation (docs/design.md §3, docs/fault-injection.md §8.2). The decision precedes the
// real call, so a faulted write leaves the file untouched and a faulted open creates nothing.

extern "C" {

int __real_open(const char* pathname, int flags, ...);
int __real_open64(const char* pathname, int flags, ...);
int __real___open_2(const char* pathname, int flags);
int __real___open64_2(const char* pathname, int flags);
ssize_t __real_read(int fd, void* buf, size_t count);
ssize_t __real___read_chk(int fd, void* buf, size_t count, size_t buflen);
ssize_t __real_write(int fd, const void* buf, size_t count);
int __real_fsync(int fd);

// The va_arg must be read before anything else, or the va_list use is invalid.
int __wrap_open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
#if defined(O_TMPFILE)
    const bool mode_needed = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
#else
    const bool mode_needed = (flags & O_CREAT) != 0;
#endif
    if (mode_needed) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }

    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_open(pathname, flags, mode);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::open)) {
    case cosmos::FaultKind::OpenEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break; // None, or an unknown kind: the real call goes through unchanged.
    }
    return __real_open(pathname, flags, mode);
}

// glibc substitutes __open_2/__open64_2 for open() when the call has no mode argument and
// _FORTIFY_SOURCE is on; --wrap=open does not see those symbols, so without this a fortified
// build bypasses the site silently (the same hole __read_chk and __recv_chk were).
// open64 is a separate symbol with the same site: an explicit open64() must not bypass it.
int __wrap_open64(const char* pathname, int flags, ...) {
    mode_t mode = 0;
#if defined(O_TMPFILE)
    const bool mode_needed = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
#else
    const bool mode_needed = (flags & O_CREAT) != 0;
#endif
    if (mode_needed) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }

    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_open64(pathname, flags, mode);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::open)) {
    case cosmos::FaultKind::OpenEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break; // None, or an unknown kind: the real call goes through unchanged.
    }
    return __real_open64(pathname, flags, mode);
}

int __wrap___open_2(const char* pathname, int flags) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real___open_2(pathname, flags);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::open)) {
    case cosmos::FaultKind::OpenEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break;
    }
    return __real___open_2(pathname, flags);
}

int __wrap___open64_2(const char* pathname, int flags) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real___open64_2(pathname, flags);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::open)) {
    case cosmos::FaultKind::OpenEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break;
    }
    return __real___open64_2(pathname, flags);
}

ssize_t __wrap_read(int fd, void* buf, size_t count) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_read(fd, buf, count);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_read_eligible(fd, count)) {
        return __real_read(fd, buf, count);
    }

    auto* sim = cosmos::Simulator::current();
    // Defense in depth: a virtual socket fd must never spend a Storage draw, even if the
    // static range check ever drifts. The transport has no read/write path, so it reaches
    // the host (EBADF on a kernel-unknown number) unfaulted.
    if (sim->net().state(fd).known) {
        return __real_read(fd, buf, count);
    }
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::read)) {
    case cosmos::FaultKind::ReadEio:
        errno = EIO;
        return -1;
    default:
        break;
    }
    return __real_read(fd, buf, count);
}

// glibc substitutes __read_chk for read() when the destination size is known; --wrap=read does not
// see that symbol, so without this the site is silently skipped in such builds.
ssize_t __wrap___read_chk(int fd, void* buf, size_t count, size_t buflen) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real___read_chk(fd, buf, count, buflen);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_read_eligible(fd, count)) {
        return __real___read_chk(fd, buf, count, buflen);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real___read_chk(fd, buf, count, buflen);
    }
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::read)) {
    case cosmos::FaultKind::ReadEio:
        errno = EIO;
        return -1;
    default:
        break;
    }
    return __real___read_chk(fd, buf, count, buflen);
}

ssize_t __wrap_write(int fd, const void* buf, size_t count) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_write(fd, buf, count);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_write_eligible(fd, count)) {
        return __real_write(fd, buf, count);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_write(fd, buf, count);
    }
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::write)) {
    case cosmos::FaultKind::WriteEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    case cosmos::FaultKind::ShortWrite:
        // A real partial transfer, not just a smaller number: reporting k bytes must mean k were
        // written. A 1-byte write has no legal short observable, so it degrades to complete.
        return __real_write(fd, buf, count >= 2 ? count / 2 : count);
    default:
        break;
    }
    return __real_write(fd, buf, count);
}

int __wrap_fsync(int fd) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_fsync(fd);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_fd_eligible(fd)) {
        return __real_fsync(fd);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_fsync(fd);
    }
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::fsync)) {
    case cosmos::FaultKind::FsyncEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break;
    }
    return __real_fsync(fd);
}

} // extern "C"
