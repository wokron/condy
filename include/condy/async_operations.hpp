#pragma once

#include "condy/awaiter_operations.hpp"
#include <liburing.h>

namespace condy {

inline auto async_splice(int fd_in, int64_t off_in, int fd_out, int64_t off_out,
                         unsigned int nbytes, unsigned int splice_flags) {
    return build_op_awaiter(io_uring_prep_splice, fd_in, off_in, fd_out,
                            off_out, nbytes, splice_flags);
}

inline auto async_tee(int fd_in, int fd_out, unsigned int nbytes,
                      unsigned int splice_flags) {
    return build_op_awaiter(io_uring_prep_tee, fd_in, fd_out, nbytes,
                            splice_flags);
}

inline auto async_readv(int fd, const struct iovec *iovecs, unsigned nr_vecs,
                        off_t offset) {
    return build_op_awaiter(io_uring_prep_readv, fd, iovecs, nr_vecs, offset);
}

inline auto async_writev(int fd, const struct iovec *iovecs, unsigned nr_vecs,
                         off_t offset) {
    return build_op_awaiter(io_uring_prep_writev, fd, iovecs, nr_vecs, offset);
}

inline auto async_recvmsg(int fd, struct msghdr *msg, unsigned flags) {
    return build_op_awaiter(io_uring_prep_recvmsg, fd, msg, flags);
}

inline auto async_sendmsg(int fd, const struct msghdr *msg, unsigned flags) {
    return build_op_awaiter(io_uring_prep_sendmsg, fd, msg, flags);
}

inline auto async_fsync(int fd, unsigned int fsync_flags) {
    return build_op_awaiter(io_uring_prep_fsync, fd, fsync_flags);
}

inline auto async_nop() { return build_op_awaiter(io_uring_prep_nop); }

inline auto async_timeout(__kernel_timespec *ts, unsigned count, int flags) {
    return build_op_awaiter(io_uring_prep_timeout, ts, count, flags);
}

inline auto async_accept(int sockfd, sockaddr *addr, socklen_t *addrlen,
                         int flags) {
    return build_op_awaiter(io_uring_prep_accept, sockfd, addr, addrlen, flags);
}

inline auto async_connect(int sockfd, const sockaddr *addr, socklen_t addrlen) {
    return build_op_awaiter(io_uring_prep_connect, sockfd, addr, addrlen);
}

inline auto async_fallocate(int fd, off_t offset, off_t len) {
    return build_op_awaiter(io_uring_prep_fallocate, fd, offset, len);
}

inline auto async_openat(int dfd, const char *path, int flags, mode_t mode) {
    return build_op_awaiter(io_uring_prep_openat, dfd, path, flags, mode);
}

inline auto async_close(int fd) {
    return build_op_awaiter(io_uring_prep_close, fd);
}

inline auto async_read(int fd, void *buf, size_t nbytes, off_t offset) {
    return build_op_awaiter(io_uring_prep_read, fd, buf, nbytes, offset);
}

inline auto async_write(int fd, const void *buf, size_t nbytes, off_t offset) {
    return build_op_awaiter(io_uring_prep_write, fd, buf, nbytes, offset);
}

inline auto async_statx(int dfd, const char *path, int flags, unsigned int mask,
                        struct statx *statxbuf) {
    return build_op_awaiter(io_uring_prep_statx, dfd, path, flags, mask,
                            statxbuf);
}

inline auto async_fadvise(int fd, off_t offset, off_t len, int advice) {
    return build_op_awaiter(io_uring_prep_fadvise, fd, offset, len, advice);
}

inline auto async_madvise(void *addr, size_t length, int advice) {
    return build_op_awaiter(io_uring_prep_madvise, addr, length, advice);
}

inline auto async_send(int sockfd, const void *buf, size_t len, int flags) {
    return build_op_awaiter(io_uring_prep_send, sockfd, buf, len, flags);
}

inline auto async_recv(int sockfd, void *buf, size_t len, int flags) {
    return build_op_awaiter(io_uring_prep_recv, sockfd, buf, len, flags);
}

inline auto async_openat2(int dfd, const char *path, struct open_how *how) {
    return build_op_awaiter(io_uring_prep_openat2, dfd, path, how);
}

inline auto async_shutdown(int fd, int how) {
    return build_op_awaiter(io_uring_prep_shutdown, fd, how);
}

inline auto async_unlinkat(int dfd, const char *path, int flags) {
    return build_op_awaiter(io_uring_prep_unlinkat, dfd, path, flags);
}

inline auto async_renameat(int olddfd, const char *oldpath, int newdfd,
                           const char *newpath) {
    return build_op_awaiter(io_uring_prep_renameat, olddfd, oldpath, newdfd,
                            newpath);
}

inline auto async_sync_file_range(int fd, off64_t offset, off64_t nbytes,
                                  unsigned int flags) {
    return build_op_awaiter(io_uring_prep_sync_file_range, fd, offset, nbytes,
                            flags);
}

inline auto async_mkdirat(int dfd, const char *path, mode_t mode) {
    return build_op_awaiter(io_uring_prep_mkdirat, dfd, path, mode);
}

inline auto async_symlinkat(const char *target, int newdirfd,
                            const char *linkpath) {
    return build_op_awaiter(io_uring_prep_symlinkat, target, newdirfd,
                            linkpath);
}

inline auto async_linkat(int olddirfd, const char *oldpath, int newdirfd,
                         const char *newpath, int flags) {
    return build_op_awaiter(io_uring_prep_linkat, olddirfd, oldpath, newdirfd,
                            newpath, flags);
}

} // namespace condy