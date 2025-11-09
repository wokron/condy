#pragma once

#include "condy/awaiter_operations.hpp"
#include "condy/condy_uring.hpp"

namespace condy {

inline auto async_splice(int fd_in, int64_t off_in, int fd_out, int64_t off_out,
                         unsigned int nbytes, unsigned int splice_flags) {
    return make_op_awaiter(io_uring_prep_splice, fd_in, off_in, fd_out, off_out,
                           nbytes, splice_flags);
}

inline auto async_tee(int fd_in, int fd_out, unsigned int nbytes,
                      unsigned int splice_flags) {
    return make_op_awaiter(io_uring_prep_tee, fd_in, fd_out, nbytes,
                           splice_flags);
}

inline auto async_readv(int fd, const struct iovec *iovecs, unsigned nr_vecs,
                        off_t offset) {
    return make_op_awaiter(io_uring_prep_readv, fd, iovecs, nr_vecs, offset);
}

inline auto async_read_fixed(int fd, void *buf, unsigned nbytes, off_t offset,
                             int buf_index) {
    return make_op_awaiter(io_uring_prep_read_fixed, fd, buf, nbytes, offset,
                           buf_index);
}

inline auto async_writev(int fd, const struct iovec *iovecs, unsigned nr_vecs,
                         off_t offset) {
    return make_op_awaiter(io_uring_prep_writev, fd, iovecs, nr_vecs, offset);
}

inline auto async_write_fixed(int fd, const void *buf, unsigned nbytes,
                              off_t offset, int buf_index) {
    return make_op_awaiter(io_uring_prep_write_fixed, fd, buf, nbytes, offset,
                           buf_index);
}

inline auto async_recvmsg(int fd, struct msghdr *msg, unsigned flags) {
    return make_op_awaiter(io_uring_prep_recvmsg, fd, msg, flags);
}

inline auto async_sendmsg(int fd, const struct msghdr *msg, unsigned flags) {
    return make_op_awaiter(io_uring_prep_sendmsg, fd, msg, flags);
}

inline auto async_poll_add(int fd, unsigned int poll_events) {
    return make_op_awaiter(io_uring_prep_poll_add, fd, poll_events);
}

template <typename CoroFunc>
inline auto async_poll_multishot(int fd, unsigned int poll_events,
                                 CoroFunc &&coro_func) {
    return make_multishot_op_awaiter_coro(std::forward<CoroFunc>(coro_func),
                                          io_uring_prep_poll_multishot, fd,
                                          poll_events);
}

inline auto async_poll_remove(uint64_t user_data) {
    return make_op_awaiter(io_uring_prep_poll_remove, user_data);
}

inline auto async_poll_update(uint64_t old_user_data, uint64_t new_user_data,
                              unsigned int poll_events, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_poll_update, old_user_data,
                           new_user_data, poll_events, flags);
}

inline auto async_fsync(int fd, unsigned int fsync_flags) {
    return make_op_awaiter(io_uring_prep_fsync, fd, fsync_flags);
}

inline auto async_nop() { return make_op_awaiter(io_uring_prep_nop); }

inline auto async_timeout(__kernel_timespec *ts, unsigned count, int flags) {
    return make_op_awaiter(io_uring_prep_timeout, ts, count, flags);
}

inline auto async_timeout_remove(uint64_t user_data, unsigned flags) {
    return make_op_awaiter(io_uring_prep_timeout_remove, user_data, flags);
}

inline auto async_timeout_update(__kernel_timespec *ts, uint64_t user_data,
                                 unsigned flags) {
    return make_op_awaiter(io_uring_prep_timeout_update, ts, user_data, flags);
}

inline auto async_accept(int sockfd, sockaddr *addr, socklen_t *addrlen,
                         int flags) {
    return make_op_awaiter(io_uring_prep_accept, sockfd, addr, addrlen, flags);
}

inline auto async_accept_direct(int sockfd, sockaddr *addr, socklen_t *addrlen,
                                int flags, unsigned int file_index) {
    return make_op_awaiter(io_uring_prep_accept_direct, sockfd, addr, addrlen,
                           flags, file_index);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
template <typename CoroFunc>
inline auto async_multishot_accept(int fd, struct sockaddr *addr,
                                   socklen_t *addrlen, int flags,
                                   CoroFunc &&coro_func) {
    return make_multishot_op_awaiter_coro(std::forward<CoroFunc>(coro_func),
                                          io_uring_prep_multishot_accept, fd,
                                          addr, addrlen, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
template <typename CoroFunc>
inline auto async_multishot_accept_direct(int fd, struct sockaddr *addr,
                                          socklen_t *addrlen, int flags,
                                          CoroFunc &&coro_func) {
    return make_multishot_op_awaiter_coro(std::forward<CoroFunc>(coro_func),
                                          io_uring_prep_multishot_accept_direct,
                                          fd, addr, addrlen, flags);
}
#endif

inline auto async_cancel(void *user_data, int flags) {
    return make_op_awaiter(io_uring_prep_cancel, user_data, flags);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_cancel_fd(int fd, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_cancel_fd, fd, flags);
}
#endif

inline auto async_link_timeout(__kernel_timespec *ts, unsigned flags) {
    return make_op_awaiter(io_uring_prep_link_timeout, ts, flags);
}

inline auto async_connect(int sockfd, const sockaddr *addr, socklen_t addrlen) {
    return make_op_awaiter(io_uring_prep_connect, sockfd, addr, addrlen);
}

inline auto async_files_update(int *fds, unsigned nr_fds, int offset) {
    return make_op_awaiter(io_uring_prep_files_update, fds, nr_fds, offset);
}

inline auto async_fallocate(int fd, off_t offset, off_t len) {
    return make_op_awaiter(io_uring_prep_fallocate, fd, offset, len);
}

inline auto async_openat(int dfd, const char *path, int flags, mode_t mode) {
    return make_op_awaiter(io_uring_prep_openat, dfd, path, flags, mode);
}

inline auto async_openat_direct(int dfd, const char *path, int flags,
                                mode_t mode, unsigned file_index) {
    return make_op_awaiter(io_uring_prep_openat_direct, dfd, path, flags, mode,
                           file_index);
}

inline auto async_close(int fd) {
    return make_op_awaiter(io_uring_prep_close, fd);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_close_direct(unsigned file_index) {
    return make_op_awaiter(io_uring_prep_close_direct, file_index);
}
#endif

inline auto async_read(int fd, void *buf, size_t nbytes, off_t offset) {
    return make_op_awaiter(io_uring_prep_read, fd, buf, nbytes, offset);
}

inline auto async_write(int fd, const void *buf, size_t nbytes, off_t offset) {
    return make_op_awaiter(io_uring_prep_write, fd, buf, nbytes, offset);
}

inline auto async_statx(int dfd, const char *path, int flags, unsigned int mask,
                        struct statx *statxbuf) {
    return make_op_awaiter(io_uring_prep_statx, dfd, path, flags, mask,
                           statxbuf);
}

inline auto async_fadvise(int fd, off_t offset, off_t len, int advice) {
    return make_op_awaiter(io_uring_prep_fadvise, fd, offset, len, advice);
}

inline auto async_madvise(void *addr, size_t length, int advice) {
    return make_op_awaiter(io_uring_prep_madvise, addr, length, advice);
}

inline auto async_send(int sockfd, const void *buf, size_t len, int flags) {
    return make_op_awaiter(io_uring_prep_send, sockfd, buf, len, flags);
}

inline auto async_recv(int sockfd, void *buf, size_t len, int flags) {
    return make_op_awaiter(io_uring_prep_recv, sockfd, buf, len, flags);
}

inline auto async_openat2(int dfd, const char *path, struct open_how *how) {
    return make_op_awaiter(io_uring_prep_openat2, dfd, path, how);
}

inline auto async_openat2_direct(int dfd, const char *path,
                                 struct open_how *how, unsigned file_index) {
    return make_op_awaiter(io_uring_prep_openat2_direct, dfd, path, how,
                           file_index);
}

inline auto async_epoll_ctl(int epfd, int fd, int op, struct epoll_event *ev) {
    return make_op_awaiter(io_uring_prep_epoll_ctl, epfd, fd, op, ev);
}

inline auto async_provide_buffers(void *addr, int len, int nr, int bgid,
                                  int bid) {
    return make_op_awaiter(io_uring_prep_provide_buffers, addr, len, nr, bgid,
                           bid);
}

inline auto async_remove_buffers(int nr, int bgid) {
    return make_op_awaiter(io_uring_prep_remove_buffers, nr, bgid);
}

inline auto async_shutdown(int fd, int how) {
    return make_op_awaiter(io_uring_prep_shutdown, fd, how);
}

inline auto async_unlinkat(int dfd, const char *path, int flags) {
    return make_op_awaiter(io_uring_prep_unlinkat, dfd, path, flags);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_unlink(const char *path, int flags) {
    return make_op_awaiter(io_uring_prep_unlink, path, flags);
}
#endif

inline auto async_renameat(int olddfd, const char *oldpath, int newdfd,
                           const char *newpath) {
    return make_op_awaiter(io_uring_prep_renameat, olddfd, oldpath, newdfd,
                           newpath);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_rename(const char *oldpath, const char *newpath) {
    return make_op_awaiter(io_uring_prep_rename, oldpath, newpath);
}
#endif

inline auto async_sync_file_range(int fd, off64_t offset, off64_t nbytes,
                                  unsigned int flags) {
    return make_op_awaiter(io_uring_prep_sync_file_range, fd, offset, nbytes,
                           flags);
}

inline auto async_mkdirat(int dfd, const char *path, mode_t mode) {
    return make_op_awaiter(io_uring_prep_mkdirat, dfd, path, mode);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_mkdir(const char *path, mode_t mode) {
    return make_op_awaiter(io_uring_prep_mkdir, path, mode);
}
#endif

inline auto async_symlinkat(const char *target, int newdirfd,
                            const char *linkpath) {
    return make_op_awaiter(io_uring_prep_symlinkat, target, newdirfd, linkpath);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_symlink(const char *target, const char *linkpath) {
    return make_op_awaiter(io_uring_prep_symlink, target, linkpath);
}
#endif

inline auto async_linkat(int olddirfd, const char *oldpath, int newdirfd,
                         const char *newpath, int flags) {
    return make_op_awaiter(io_uring_prep_linkat, olddirfd, oldpath, newdirfd,
                           newpath, flags);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_link(const char *oldpath, const char *newpath, int flags) {
    return make_op_awaiter(io_uring_prep_link, oldpath, newpath, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_msg_ring(int fd, unsigned int len, uint64_t data,
                           unsigned int flags) {
    return make_op_awaiter(io_uring_prep_msg_ring, fd, len, data, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_getxattr(const char *name, const char *path, char *value,
                           size_t len) {
    return make_op_awaiter(io_uring_prep_getxattr, name, path, value, len);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_setxattr(const char *name, const char *path,
                           const char *value, int flags, size_t len) {
    return make_op_awaiter(io_uring_prep_setxattr, name, path, value, flags,
                           len);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_fgetxattr(int fd, const char *name, char *value, size_t len) {
    return make_op_awaiter(io_uring_prep_fgetxattr, fd, name, value, len);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_fsetxattr(int fd, const char *name, const char *value,
                            int flags, size_t len) {
    return make_op_awaiter(io_uring_prep_fsetxattr, fd, name, value, flags,
                           len);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_socket(int domain, int type, int protocol,
                         unsigned int flags) {
    return make_op_awaiter(io_uring_prep_socket, domain, type, protocol, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_socket_direct(int domain, int type, int protocol,
                                unsigned file_index, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_socket_direct, domain, type, protocol,
                           file_index, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_socket_direct_alloc(int domain, int type, int protocol,
                                      unsigned int flags) {
    return make_op_awaiter(io_uring_prep_socket_direct_alloc, domain, type,
                           protocol, flags);
}
#endif

} // namespace condy