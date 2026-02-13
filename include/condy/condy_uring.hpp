/**
 * @file condy_uring.hpp
 */

#pragma once

#include <liburing.h> // IWYU pragma: export

// liburing <= 2.3 has no version macros, define them here

#ifndef IO_URING_VERSION_MAJOR
#define IO_URING_VERSION_MAJOR 2
#endif

#ifndef IO_URING_VERSION_MINOR
#define IO_URING_VERSION_MINOR 3
#endif

#ifndef IO_URING_CHECK_VERSION
#define IO_URING_CHECK_VERSION(major, minor)                                   \
    (major > IO_URING_VERSION_MAJOR ||                                         \
     (major == IO_URING_VERSION_MAJOR && minor > IO_URING_VERSION_MINOR))
#endif

// Polyfill for io_uring_prep_uring_cmd (added in liburing 2.13)
// Opcode exists since 2.3, only the helper function is missing
#if IO_URING_CHECK_VERSION(2, 13) // < 2.13
inline void io_uring_prep_uring_cmd(struct io_uring_sqe *sqe, int cmd_op,
                                    int fd) noexcept {
    sqe->opcode = (__u8)IORING_OP_URING_CMD;
    sqe->fd = fd;
    sqe->cmd_op = cmd_op;
    sqe->__pad1 = 0;
    sqe->addr = 0ul;
    sqe->len = 0;
}
#endif
