#pragma once

#include "condy/awaiter_operations.hpp"
#include "condy/buffers.hpp"
#include "condy/condy_uring.hpp"
#include <type_traits>

namespace condy {

namespace detail {

struct FixedFd {
    int value;
    operator int() const { return value; }
};

template <typename OpAwaiter>
void maybe_add_fixed_fd_flag(OpAwaiter &op, const FixedFd &fixed_fd) {
    op.add_flags(IOSQE_FIXED_FILE);
}

template <typename OpAwaiter>
void maybe_add_fixed_fd_flag(OpAwaiter &op, int fd) { /* No-op */ }

template <typename Buffer>
constexpr bool is_provided_buffers_v =
    std::is_same_v<std::decay_t<Buffer>, ProvidedBuffers>;

template <typename BufferBase> class FixedBuffer : public BufferBase {
public:
    FixedBuffer(int buf_index, BufferBase base)
        : BufferBase(base), buf_index_(buf_index) {}

    int buf_index() const { return buf_index_; }

private:
    int buf_index_;
};

template <typename Buffer> struct is_fixed_buffer : public std::false_type {};

template <typename BufferBase>
struct is_fixed_buffer<FixedBuffer<BufferBase>> : public std::true_type {};

template <typename Buffer>
constexpr bool is_fixed_buffer_v = is_fixed_buffer<std::decay_t<Buffer>>::value;

} // namespace detail

// Helper to specify a fixed fd
inline auto fixed(int fd) { return detail::FixedFd{fd}; }

// Helper to specify a fixed buffer
template <typename Buffer> auto fixed(int buf_index, Buffer &&buf) {
    return detail::FixedBuffer<std::decay_t<Buffer>>(buf_index,
                                                     std::forward<Buffer>(buf));
}

inline auto async_nop() { return make_op_awaiter(io_uring_prep_nop); }

inline auto async_timeout(struct __kernel_timespec *ts, unsigned count,
                          unsigned flags) {
    return make_op_awaiter(io_uring_prep_timeout, ts, count, flags);
}

template <typename Fd, typename Buffer>
inline auto async_read(Fd fd, Buffer &&buf, __u64 offset) {
    auto op = [&] {
        if constexpr (detail::is_provided_buffers_v<Buffer>) {
            return make_select_buffer_op_awaiter(
                std::forward<Buffer>(buf).copy_impl(), io_uring_prep_read, fd,
                nullptr, 0, offset);
        } else if constexpr (detail::is_fixed_buffer_v<Buffer>) {
            return make_op_awaiter(io_uring_prep_read_fixed, fd, buf.data(),
                                   buf.size(), offset, buf.buf_index());
        } else {
            return make_op_awaiter(io_uring_prep_read, fd, buf.data(),
                                   buf.size(), offset);
        }
    }();
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
template <typename Fd, typename Buffer, typename MultiShotFunc>
inline auto async_read_multishot(Fd fd, Buffer &&buf, __u64 offset,
                                 MultiShotFunc &&func) {
    auto prep = [](io_uring_sqe *sqe, int fd, __u64 offset) {
        io_uring_prep_rw(IORING_OP_READ_MULTISHOT, sqe, fd, nullptr, 0, offset);
    };
    auto op = make_multishot_select_buffer_op_awaiter(
        std::forward<MultiShotFunc>(func),
        std::forward<Buffer>(buf).copy_impl(), prep, fd, offset);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}
#endif

template <typename Fd, typename Buffer>
inline auto async_write(Fd fd, Buffer &&buf, __u64 offset) {
    auto op = [&] {
        if constexpr (detail::is_provided_buffers_v<Buffer>) {
            return make_select_buffer_op_awaiter(
                std::forward<Buffer>(buf).copy_impl(), io_uring_prep_write, fd,
                nullptr, 0, offset);
        } else if constexpr (detail::is_fixed_buffer_v<Buffer>) {
            return make_op_awaiter(io_uring_prep_write_fixed, fd, buf.data(),
                                   buf.size(), offset, buf.buf_index());
        } else {
            return make_op_awaiter(io_uring_prep_write, fd, buf.data(),
                                   buf.size(), offset);
        }
    }();
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

inline auto async_openat(int dfd, const char *path, int flags, mode_t mode) {
    return make_op_awaiter(io_uring_prep_openat, dfd, path, flags, mode);
}

#if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
inline auto async_openat_direct(int dfd, const char *path, int flags,
                                mode_t mode, unsigned file_index) {
    return make_op_awaiter(io_uring_prep_openat_direct, dfd, path, flags, mode,
                           file_index);
}
#endif

inline auto async_statx(int dfd, const char *path, int flags, unsigned mask,
                        struct statx *statxbuf) {
    return make_op_awaiter(io_uring_prep_statx, dfd, path, flags, mask,
                           statxbuf);
}

inline auto async_close(int fd) {
    return make_op_awaiter(io_uring_prep_close, fd);
}

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
inline auto async_close(detail::FixedFd fd) {
    return make_op_awaiter(io_uring_prep_close_direct, fd);
}
#endif

template <typename Fd>
inline auto async_accept(Fd fd, struct sockaddr *addr, socklen_t *addrlen,
                         int flags) {
    auto op = make_op_awaiter(io_uring_prep_accept, fd, addr, addrlen, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

#if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
template <typename Fd>
inline auto async_accept_direct(Fd fd, struct sockaddr *addr,
                                socklen_t *addrlen, int flags,
                                unsigned int file_index) {
    auto op = make_op_awaiter(io_uring_prep_accept_direct, fd, addr, addrlen,
                              flags, file_index);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
template <typename Fd, typename MultiShotFunc>
inline auto async_multishot_accept(Fd fd, struct sockaddr *addr,
                                   socklen_t *addrlen, int flags,
                                   MultiShotFunc &&func) {
    auto op = make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
                                        io_uring_prep_multishot_accept, fd,
                                        addr, addrlen, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}
#endif

#if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
template <typename Fd, typename MultiShotFunc>
inline auto async_multishot_accept_direct(Fd fd, struct sockaddr *addr,
                                          socklen_t *addrlen, int flags,
                                          MultiShotFunc &&func) {
    auto op = make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
                                        io_uring_prep_multishot_accept_direct,
                                        fd, addr, addrlen, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}
#endif

#if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
template <typename Fd, typename Buffer, typename FreeFunc>
inline auto async_send_zc(Fd sockfd, Buffer &&buf, int flags, unsigned zc_flags,
                          FreeFunc &&func) {
    auto op = [&] {
        if constexpr (detail::is_fixed_buffer_v<Buffer>) {
            return make_zero_copy_op_awaiter(std::forward<FreeFunc>(func),
                                             io_uring_prep_send_zc_fixed,
                                             sockfd, buf.data(), buf.size(),
                                             flags, zc_flags, buf.buf_index());
        } else {
            return make_zero_copy_op_awaiter(
                std::forward<FreeFunc>(func), io_uring_prep_send_zc, sockfd,
                buf.data(), buf.size(), flags, zc_flags);
        }
        // TODO: Support send_bundle
    }();
    detail::maybe_add_fixed_fd_flag(op, sockfd);
    return op;
}
#endif

} // namespace condy

// inline auto async_splice(int fd_in, int64_t off_in, int fd_out, int64_t
// off_out,
//                          unsigned int nbytes, unsigned int splice_flags) {
//     return make_op_awaiter(io_uring_prep_splice, fd_in, off_in, fd_out,
//     off_out,
//                            nbytes, splice_flags);
// }

// inline auto async_tee(int fd_in, int fd_out, unsigned int nbytes,
//                       unsigned int splice_flags) {
//     return make_op_awaiter(io_uring_prep_tee, fd_in, fd_out, nbytes,
//                            splice_flags);
// }

// template <typename Fd>
// inline auto async_readv(Fd fd, const struct iovec *iovecs, unsigned nr_vecs,
//                         __u64 offset) {
//     return make_op_awaiter(io_uring_prep_readv, fd, iovecs, nr_vecs, offset);
// }

// #if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
// inline auto async_readv2(int fd, const struct iovec *iovecs, unsigned
// nr_vecs,
//                          __u64 offset, int flags) {
//     return make_op_awaiter(io_uring_prep_readv2, fd, iovecs, nr_vecs, offset,
//                            flags);
// }
// #endif

// inline auto async_read_fixed(int fd, void *buf, unsigned nbytes, __u64
// offset,
//                              int buf_index) {
//     return make_op_awaiter(io_uring_prep_read_fixed, fd, buf, nbytes, offset,
//                            buf_index);
// }

// #if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
// inline auto async_readv_fixed(int fd, const struct iovec *iovecs,
//                               unsigned nr_vecs, __u64 offset, int flags,
//                               int buf_index) {
//     return make_op_awaiter(io_uring_prep_readv_fixed, fd, iovecs, nr_vecs,
//                            offset, flags, buf_index);
// }
// #endif

// inline auto async_writev(int fd, const struct iovec *iovecs, unsigned
// nr_vecs,
//                          __u64 offset) {
//     return make_op_awaiter(io_uring_prep_writev, fd, iovecs, nr_vecs,
//     offset);
// }

// #if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
// inline auto async_writev2(int fd, const struct iovec *iovecs, unsigned
// nr_vecs,
//                           __u64 offset, int flags) {
//     return make_op_awaiter(io_uring_prep_writev2, fd, iovecs, nr_vecs,
//     offset,
//                            flags);
// }
// #endif

// inline auto async_write_fixed(int fd, const void *buf, unsigned nbytes,
//                               __u64 offset, int buf_index) {
//     return make_op_awaiter(io_uring_prep_write_fixed, fd, buf, nbytes,
//     offset,
//                            buf_index);
// }

// #if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
// inline auto async_writev_fixed(int fd, const struct iovec *iovecs,
//                                unsigned nr_vecs, __u64 offset, int flags,
//                                int buf_index) {
//     return make_op_awaiter(io_uring_prep_writev_fixed, fd, iovecs, nr_vecs,
//                            offset, flags, buf_index);
// }
// #endif

// inline auto async_recvmsg(int fd, struct msghdr *msg, unsigned flags) {
//     return make_op_awaiter(io_uring_prep_recvmsg, fd, msg, flags);
// }

// #if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
// template <typename MultiShotFunc>
// inline auto async_recvmsg_multishot(int fd, struct msghdr *msg, unsigned
// flags,
//                                     MultiShotFunc &&func) {
//     return make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
//                                      io_uring_prep_recvmsg_multishot, fd,
//                                      msg, flags);
// }
// #endif

// inline auto async_sendmsg(int fd, const struct msghdr *msg, unsigned flags) {
//     return make_op_awaiter(io_uring_prep_sendmsg, fd, msg, flags);
// }

// inline auto async_poll_add(int fd, unsigned poll_mask) {
//     return make_op_awaiter(io_uring_prep_poll_add, fd, poll_mask);
// }

// #if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
// template <typename MultiShotFunc>
// inline auto async_poll_multishot(int fd, unsigned poll_mask,
//                                  MultiShotFunc &&func) {
//     return make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
//                                      io_uring_prep_poll_multishot, fd,
//                                      poll_mask);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
// inline auto async_poll_remove(__u64 user_data) {
//     return make_op_awaiter(io_uring_prep_poll_remove, user_data);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
// inline auto async_poll_update(__u64 old_user_data, __u64 new_user_data,
//                               unsigned poll_mask, unsigned flags) {
//     return make_op_awaiter(io_uring_prep_poll_update, old_user_data,
//                            new_user_data, poll_mask, flags);
// }
// #endif

// inline auto async_fsync(int fd, unsigned fsync_flags) {
//     return make_op_awaiter(io_uring_prep_fsync, fd, fsync_flags);
// }

// inline auto async_nop() { return make_op_awaiter(io_uring_prep_nop); }

// inline auto async_timeout(struct __kernel_timespec *ts, unsigned count,
//                           unsigned flags) {
//     return make_op_awaiter(io_uring_prep_timeout, ts, count, flags);
// }

// inline auto async_timeout_remove(__u64 user_data, unsigned flags) {
//     return make_op_awaiter(io_uring_prep_timeout_remove, user_data, flags);
// }

// inline auto async_timeout_update(struct __kernel_timespec *ts, __u64
// user_data,
//                                  unsigned flags) {
//     return make_op_awaiter(io_uring_prep_timeout_update, ts, user_data,
//     flags);
// }

// inline auto async_accept(int fd, struct sockaddr *addr, socklen_t *addrlen,
//                          int flags) {
//     return make_op_awaiter(io_uring_prep_accept, fd, addr, addrlen, flags);
// }

// #if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
// inline auto async_accept_direct(int fd, struct sockaddr *addr,
//                                 socklen_t *addrlen, int flags,
//                                 unsigned int file_index) {
//     return make_op_awaiter(io_uring_prep_accept_direct, fd, addr, addrlen,
//                            flags, file_index);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// template <typename MultiShotFunc>
// inline auto async_multishot_accept(int fd, struct sockaddr *addr,
//                                    socklen_t *addrlen, int flags,
//                                    MultiShotFunc &&func) {
//     return make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
//                                      io_uring_prep_multishot_accept, fd,
//                                      addr, addrlen, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// template <typename MultiShotFunc>
// inline auto async_multishot_accept_direct(int fd, struct sockaddr *addr,
//                                           socklen_t *addrlen, int flags,
//                                           MultiShotFunc &&func) {
//     return make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
//                                      io_uring_prep_multishot_accept_direct,
//                                      fd, addr, addrlen, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
// inline auto async_cancel64(__u64 user_data, int flags) {
//     return make_op_awaiter(io_uring_prep_cancel64, user_data, flags);
// }
// #endif

// inline auto async_cancel(void *user_data, int flags) {
//     return make_op_awaiter(io_uring_prep_cancel, user_data, flags);
// }

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_cancel_fd(int fd, unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_cancel_fd, fd, flags);
// }
// #endif

// inline auto async_link_timeout(struct __kernel_timespec *ts, unsigned flags)
// {
//     return make_op_awaiter(io_uring_prep_link_timeout, ts, flags);
// }

// inline auto async_connect(int fd, const struct sockaddr *addr,
//                           socklen_t addrlen) {
//     return make_op_awaiter(io_uring_prep_connect, fd, addr, addrlen);
// }

// #if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
// inline auto async_bind(int fd, struct sockaddr *addr, socklen_t addrlen) {
//     return make_op_awaiter(io_uring_prep_bind, fd, addr, addrlen);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
// inline auto async_listen(int fd, int backlog) {
//     return make_op_awaiter(io_uring_prep_listen, fd, backlog);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
// inline auto async_epoll_wait(int fd, struct epoll_event *events, int
// maxevents,
//                              unsigned flags) {
//     return make_op_awaiter(io_uring_prep_epoll_wait, fd, events, maxevents,
//                            flags);
// }
// #endif

// inline auto async_files_update(int *fds, unsigned nr_fds, int offset) {
//     return make_op_awaiter(io_uring_prep_files_update, fds, nr_fds, offset);
// }

// inline auto async_fallocate(int fd, int mode, __u64 offset, __u64 len) {
//     return make_op_awaiter(io_uring_prep_fallocate, fd, mode, offset, len);
// }

// inline auto async_openat(int dfd, const char *path, int flags, mode_t mode) {
//     return make_op_awaiter(io_uring_prep_openat, dfd, path, flags, mode);
// }

// #if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
// inline auto async_openat_direct(int dfd, const char *path, int flags,
//                                 mode_t mode, unsigned file_index) {
//     return make_op_awaiter(io_uring_prep_openat_direct, dfd, path, flags,
//     mode,
//                            file_index);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
// inline auto async_open(const char *path, int flags, mode_t mode) {
//     return make_op_awaiter(io_uring_prep_open, path, flags, mode);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
// inline auto async_open_direct(const char *path, int flags, mode_t mode,
//                               unsigned file_index) {
//     return make_op_awaiter(io_uring_prep_open_direct, path, flags, mode,
//                            file_index);
// }
// #endif

// inline auto async_close(int fd) {
//     return make_op_awaiter(io_uring_prep_close, fd);
// }

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_close_direct(unsigned file_index) {
//     return make_op_awaiter(io_uring_prep_close_direct, file_index);
// }
// #endif

// inline auto async_read(int fd, void *buf, unsigned nbytes, __u64 offset) {
//     return make_op_awaiter(io_uring_prep_read, fd, buf, nbytes, offset);
// }

// #if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
// template <typename MultiShotFunc>
// inline auto async_read_multishot(int fd, unsigned nbytes, __u64 offset,
//                                  int buf_group, MultiShotFunc &&func) {
//     return make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
//                                      io_uring_prep_read_multishot, fd,
//                                      nbytes, offset, buf_group);
// }
// #endif

// inline auto async_write(int fd, const void *buf, unsigned nbytes,
//                         __u64 offset) {
//     return make_op_awaiter(io_uring_prep_write, fd, buf, nbytes, offset);
// }

// inline auto async_statx(int dfd, const char *path, int flags, unsigned mask,
//                         struct statx *statxbuf) {
//     return make_op_awaiter(io_uring_prep_statx, dfd, path, flags, mask,
//                            statxbuf);
// }

// inline auto async_fadvise(int fd, __u64 offset, __u32 len, int advice) {
//     return make_op_awaiter(io_uring_prep_fadvise, fd, offset, len, advice);
// }

// inline auto async_madvise(void *addr, __u32 length, int advice) {
//     return make_op_awaiter(io_uring_prep_madvise, addr, length, advice);
// }

// #if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
// inline auto async_fadvise64(int fd, __u64 offset, off_t len, int advice) {
//     return make_op_awaiter(io_uring_prep_fadvise64, fd, offset, len, advice);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
// inline auto async_madvise64(void *addr, off_t length, int advice) {
//     return make_op_awaiter(io_uring_prep_madvise64, addr, length, advice);
// }
// #endif

// inline auto async_send(int sockfd, const void *buf, size_t len, int flags) {
//     return make_op_awaiter(io_uring_prep_send, sockfd, buf, len, flags);
// }

// #if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
// inline auto async_send_bundle(int sockfd, size_t len, int flags) {
//     return make_op_awaiter(io_uring_prep_send_bundle, sockfd, len, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
// inline auto async_sendto(int sockfd, const void *buf, size_t len, int flags,
//                          const struct sockaddr *addr, socklen_t addrlen) {
//     return make_op_awaiter(io_uring_prep_sendto, sockfd, buf, len, flags,
//     addr,
//                            addrlen);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
// inline auto async_send_zc(int sockfd, const void *buf, size_t len, int flags,
//                           unsigned zc_flags) {
//     return make_op_awaiter(io_uring_prep_send_zc, sockfd, buf, len, flags,
//                            zc_flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
// inline auto async_send_zc_fixed(int sockfd, const void *buf, size_t len,
//                                 int flags, unsigned zc_flags,
//                                 unsigned buf_index) {
//     return make_op_awaiter(io_uring_prep_send_zc_fixed, sockfd, buf, len,
//     flags,
//                            zc_flags, buf_index);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
// inline auto async_sendmsg_zc(int fd, const struct msghdr *msg, unsigned
// flags) {
//     return make_op_awaiter(io_uring_prep_sendmsg_zc, fd, msg, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
// inline auto async_sendmsg_zc_fixed(int fd, const struct msghdr *msg,
//                                    unsigned flags, unsigned buf_index) {
//     return make_op_awaiter(io_uring_prep_sendmsg_zc_fixed, fd, msg, flags,
//                            buf_index);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
// inline auto async_send_set_addr(const struct sockaddr *dest_addr,
//                                 __u16 addr_len) {
//     return make_op_awaiter(io_uring_prep_send_set_addr, dest_addr, addr_len);
// }
// #endif

// inline auto async_recv(int sockfd, void *buf, size_t len, int flags) {
//     return make_op_awaiter(io_uring_prep_recv, sockfd, buf, len, flags);
// }

// #if !IO_URING_CHECK_VERSION(2, 3) // >= 2.3
// template <typename MultiShotFunc>
// inline auto async_recv_multishot(int sockfd, void *buf, size_t len, int
// flags,
//                                  MultiShotFunc &&func) {
//     return make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
//                                      io_uring_prep_recv_multishot, sockfd,
//                                      buf, len, flags);
// }
// #endif

// inline auto async_openat2(int dfd, const char *path, struct open_how *how) {
//     return make_op_awaiter(io_uring_prep_openat2, dfd, path, how);
// }

// #if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
// inline auto async_openat2_direct(int dfd, const char *path,
//                                  struct open_how *how, unsigned file_index) {
//     return make_op_awaiter(io_uring_prep_openat2_direct, dfd, path, how,
//                            file_index);
// }
// #endif

// inline auto async_epoll_ctl(int epfd, int fd, int op, struct epoll_event *ev)
// {
//     return make_op_awaiter(io_uring_prep_epoll_ctl, epfd, fd, op, ev);
// }

// inline auto async_provide_buffers(void *addr, int len, int nr, int bgid,
//                                   int bid) {
//     return make_op_awaiter(io_uring_prep_provide_buffers, addr, len, nr,
//     bgid,
//                            bid);
// }

// inline auto async_remove_buffers(int nr, int bgid) {
//     return make_op_awaiter(io_uring_prep_remove_buffers, nr, bgid);
// }

// inline auto async_shutdown(int fd, int how) {
//     return make_op_awaiter(io_uring_prep_shutdown, fd, how);
// }

// inline auto async_unlinkat(int dfd, const char *path, int flags) {
//     return make_op_awaiter(io_uring_prep_unlinkat, dfd, path, flags);
// }

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_unlink(const char *path, int flags) {
//     return make_op_awaiter(io_uring_prep_unlink, path, flags);
// }
// #endif

// inline auto async_renameat(int olddfd, const char *oldpath, int newdfd,
//                            const char *newpath, unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_renameat, olddfd, oldpath, newdfd,
//                            newpath, flags);
// }

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_rename(const char *oldpath, const char *newpath) {
//     return make_op_awaiter(io_uring_prep_rename, oldpath, newpath);
// }
// #endif

// inline auto async_sync_file_range(int fd, unsigned len, __u64 offset,
//                                   int flags) {
//     return make_op_awaiter(io_uring_prep_sync_file_range, fd, len, offset,
//                            flags);
// }

// #if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
// inline auto async_mkdirat(int dfd, const char *path, mode_t mode) {
//     return make_op_awaiter(io_uring_prep_mkdirat, dfd, path, mode);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_mkdir(const char *path, mode_t mode) {
//     return make_op_awaiter(io_uring_prep_mkdir, path, mode);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
// inline auto async_symlinkat(const char *target, int newdirfd,
//                             const char *linkpath) {
//     return make_op_awaiter(io_uring_prep_symlinkat, target, newdirfd,
//     linkpath);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_symlink(const char *target, const char *linkpath) {
//     return make_op_awaiter(io_uring_prep_symlink, target, linkpath);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 1) // >= 2.1
// inline auto async_linkat(int olddfd, const char *oldpath, int newdfd,
//                          const char *newpath, int flags) {
//     return make_op_awaiter(io_uring_prep_linkat, olddfd, oldpath, newdfd,
//                            newpath, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_link(const char *oldpath, const char *newpath, int flags) {
//     return make_op_awaiter(io_uring_prep_link, oldpath, newpath, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
// inline auto async_msg_ring_cqe_flags(int fd, unsigned int len, __u64 data,
//                                      unsigned int flags,
//                                      unsigned int cqe_flags) {
//     return make_op_awaiter(io_uring_prep_msg_ring_cqe_flags, fd, len, data,
//                            flags, cqe_flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_msg_ring(int fd, unsigned int len, __u64 data,
//                            unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_msg_ring, fd, len, data, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
// inline auto async_msg_ring_fd(int fd, int source_fd, int target_fd, __u64
// data,
//                               unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_msg_ring_fd, fd, source_fd,
//     target_fd,
//                            data, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
// inline auto async_msg_ring_fd_alloc(int fd, int source_fd, __u64 data,
//                                     unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_msg_ring_fd_alloc, fd, source_fd,
//     data,
//                            flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_getxattr(const char *name, char *value, const char *path,
//                            unsigned int len) {
//     return make_op_awaiter(io_uring_prep_getxattr, name, value, path, len);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_setxattr(const char *name, const char *value,
//                            const char *path, int flags, unsigned int len) {
//     return make_op_awaiter(io_uring_prep_setxattr, name, value, path, flags,
//                            len);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_fgetxattr(int fd, const char *name, char *value,
//                             unsigned int len) {
//     return make_op_awaiter(io_uring_prep_fgetxattr, fd, name, value, len);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_fsetxattr(int fd, const char *name, const char *value,
//                             int flags, unsigned int len) {
//     return make_op_awaiter(io_uring_prep_fsetxattr, fd, name, value, flags,
//                            len);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_socket(int domain, int type, int protocol,
//                          unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_socket, domain, type, protocol,
//     flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_socket_direct(int domain, int type, int protocol,
//                                 unsigned file_index, unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_socket_direct, domain, type,
//     protocol,
//                            file_index, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 2) // >= 2.2
// inline auto async_socket_direct_alloc(int domain, int type, int protocol,
//                                       unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_socket_direct_alloc, domain, type,
//                            protocol, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
// inline auto async_cmd_sock(int cmd_op, int fd, int level, int optname,
//                            void *optval, int optlen) {
//     return make_op_awaiter(io_uring_prep_cmd_sock, cmd_op, fd, level,
//     optname,
//                            optval, optlen);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
// inline auto async_waitid(idtype_t idtype, id_t id, siginfo_t *infop,
//                          int options, unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_waitid, idtype, id, infop, options,
//                            flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
// inline auto async_futex_wake(uint32_t *futex, uint64_t val, uint64_t mask,
//                              uint32_t futex_flags, unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_futex_wake, futex, val, mask,
//                            futex_flags, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
// inline auto async_futex_wait(uint32_t *futex, uint64_t val, uint64_t mask,
//                              uint32_t futex_flags, unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_futex_wait, futex, val, mask,
//                            futex_flags, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
// inline auto async_futex_waitv(struct futex_waitv *futex, uint32_t nr_futex,
//                               unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_futex_waitv, futex, nr_futex,
//     flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
// inline auto async_fixed_fd_install(int fd, unsigned int flags) {
//     return make_op_awaiter(io_uring_prep_fixed_fd_install, fd, flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
// inline auto async_ftruncate(int fd, loff_t len) {
//     return make_op_awaiter(io_uring_prep_ftruncate, fd, len);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
// inline auto async_cmd_discard(int fd, uint64_t offset, uint64_t nbytes) {
//     return make_op_awaiter(io_uring_prep_cmd_discard, fd, offset, nbytes);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
// inline auto async_pipe(int *fds, int pipe_flags) {
//     return make_op_awaiter(io_uring_prep_pipe, fds, pipe_flags);
// }
// #endif

// #if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
// inline auto async_pipe_direct(int *fds, int pipe_flags,
//                               unsigned int file_index) {
//     return make_op_awaiter(io_uring_prep_pipe_direct, fds, pipe_flags,
//                            file_index);
// }
// #endif
