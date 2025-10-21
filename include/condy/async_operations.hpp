#pragma once

#include "condy/awaiter_operations.hpp"
#include <liburing.h>

namespace condy {

inline auto async_nop() { return build_op_awaiter(io_uring_prep_nop); }

inline auto async_accept(int sockfd, sockaddr *addr, socklen_t *addrlen,
                         int flags) {
    return build_op_awaiter(io_uring_prep_accept, sockfd, addr, addrlen, flags);
}

inline auto async_connect(int sockfd, const sockaddr *addr, socklen_t addrlen) {
    return build_op_awaiter(io_uring_prep_connect, sockfd, addr, addrlen);
}

inline auto async_read(int fd, void *buf, size_t nbytes, off_t offset) {
    return build_op_awaiter(io_uring_prep_read, fd, buf, nbytes, offset);
}

inline auto async_write(int fd, const void *buf, size_t nbytes, off_t offset) {
    return build_op_awaiter(io_uring_prep_write, fd, buf, nbytes, offset);
}

inline auto async_close(int fd) {
    return build_op_awaiter(io_uring_prep_close, fd);
}

// TODO: More async operations...

} // namespace condy