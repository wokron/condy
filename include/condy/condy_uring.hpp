#pragma once

#include <liburing.h> // IWYU pragma: export

// Earlier liburing has no version macros, define our own
#ifndef CONDY_IO_URING_VERSION_MAJOR
#define CONDY_IO_URING_VERSION_MAJOR 2
#endif

#ifndef CONDY_IO_URING_VERSION_MINOR
#define CONDY_IO_URING_VERSION_MINOR 1
#endif

#ifndef IO_URING_VERSION_MAJOR
#define IO_URING_VERSION_MAJOR CONDY_IO_URING_VERSION_MAJOR
#endif

#ifndef IO_URING_VERSION_MINOR
#define IO_URING_VERSION_MINOR CONDY_IO_URING_VERSION_MINOR
#endif

#ifndef IO_URING_CHECK_VERSION
#define IO_URING_CHECK_VERSION(major, minor)                                   \
    (major > IO_URING_VERSION_MAJOR ||                                         \
     (major == IO_URING_VERSION_MAJOR && minor > IO_URING_VERSION_MINOR))
#endif

// Version independent wrappers

inline int condy_submit_and_wait_timeout(struct io_uring *ring,
                                         struct io_uring_cqe **cqe_ptr,
                                         unsigned wait_nr,
                                         struct __kernel_timespec *ts,
                                         sigset_t *sigmask) {
#if IO_URING_CHECK_VERSION(2, 1)
    return io_uring_submit_and_wait_timeout(ring, cqe_ptr, wait_nr, ts,
                                            sigmask);
#else
    if (ts == nullptr) {
        return io_uring_submit_and_wait(ring, wait_nr);
    }
    io_uring_submit(ring);
    return io_uring_wait_cqes(ring, cqe_ptr, wait_nr, ts, sigmask);
#endif
}