/**
 * @file async_operations.hpp
 * @brief Definitions of asynchronous operations.
 * @details This file defines a series of asynchronous operations, which are
 * wrappers around liburing operations. Generally, each async_* function
 * corresponds to a io_uring_prep_* function in liburing
 */

#pragma once

#include "condy/awaiter_operations.hpp"
#include "condy/concepts.hpp"
#include "condy/condy_uring.hpp"
#include "condy/helpers.hpp"
#include "condy/provided_buffers.hpp"

namespace condy {

namespace detail {

template <AwaiterLike Awaiter>
auto maybe_flag_fixed_fd(Awaiter &&op, const FixedFd &) {
    return flag<IOSQE_FIXED_FILE>(std::forward<Awaiter>(op));
}

template <AwaiterLike Awaiter> auto maybe_flag_fixed_fd(Awaiter &&op, int) {
    return std::forward<Awaiter>(op);
}

template <typename Fd>
constexpr bool is_fixed_fd_v = std::is_same_v<std::decay_t<Fd>, FixedFd>;

} // namespace detail

/**
 * @brief See io_uring_prep_splice
 */
template <FdLike Fd1, FdLike Fd2>
inline auto async_splice(Fd1 fd_in, int64_t off_in, Fd2 fd_out, int64_t off_out,
                         unsigned int nbytes, unsigned int splice_flags) {
    if constexpr (detail::is_fixed_fd_v<Fd1>) {
        splice_flags |= SPLICE_F_FD_IN_FIXED;
    }
    auto op = make_op_awaiter(io_uring_prep_splice, fd_in, off_in, fd_out,
                              off_out, nbytes, splice_flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd_out);
}

/**
 * @brief See io_uring_prep_tee
 */
template <FdLike Fd1, FdLike Fd2>
inline auto async_tee(Fd1 fd_in, Fd2 fd_out, unsigned int nbytes,
                      unsigned int splice_flags) {
    if constexpr (detail::is_fixed_fd_v<Fd1>) {
        splice_flags |= SPLICE_F_FD_IN_FIXED;
    }
    auto op =
        make_op_awaiter(io_uring_prep_tee, fd_in, fd_out, nbytes, splice_flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd_out);
}

/**
 * @brief See io_uring_prep_readv2
 */
template <FdLike Fd>
inline auto async_readv(Fd fd, const struct iovec *iovecs, unsigned nr_vecs,
                        __u64 offset, int flags) {
    auto op = make_op_awaiter(io_uring_prep_readv2, fd, iovecs, nr_vecs, offset,
                              flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
/**
 * @brief See io_uring_prep_readv2
 */
template <FdLike Fd>
inline auto async_readv(Fd fd, detail::FixedBuffer<const iovec *> iovecs,
                        unsigned nr_vecs, __u64 offset, int flags) {
    auto op = make_op_awaiter(io_uring_prep_readv_fixed, fd, iovecs.value,
                              nr_vecs, offset, flags, iovecs.buf_index);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

template <FdLike Fd>
/**
 * @brief See io_uring_prep_writev2
 */
inline auto async_writev(Fd fd, const struct iovec *iovecs,
                         unsigned int nr_vecs, __u64 offset, int flags) {
    auto op = make_op_awaiter(io_uring_prep_writev2, fd, iovecs, nr_vecs,
                              offset, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
/**
 * @brief See io_uring_prep_writev2
 */
template <FdLike Fd>
inline auto async_writev(Fd fd, detail::FixedBuffer<const iovec *> iovecs,
                         unsigned int nr_vecs, __u64 offset, int flags) {
    auto op = make_op_awaiter(io_uring_prep_writev_fixed, fd, iovecs.value,
                              nr_vecs, offset, flags, iovecs.buf_index);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

/**
 * @brief See io_uring_prep_recvmsg
 */
template <FdLike Fd>
inline auto async_recvmsg(Fd fd, struct msghdr *msg, unsigned flags) {
    auto op = make_op_awaiter(io_uring_prep_recvmsg, fd, msg, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_recvmsg_multishot
 */
template <FdLike Fd, typename MultiShotFunc, NotBundledBufferRing Buffer>
inline auto async_recvmsg_multishot(Fd fd, struct msghdr *msg, unsigned flags,
                                    Buffer &buf, MultiShotFunc &&func) {
    auto op = make_multishot_select_buffer_op_awaiter(
        std::forward<MultiShotFunc>(func), &buf,
        io_uring_prep_recvmsg_multishot, fd, msg, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_sendmsg
 */
template <FdLike Fd>
inline auto async_sendmsg(Fd fd, const struct msghdr *msg, unsigned flags) {
    auto op = make_op_awaiter(io_uring_prep_sendmsg, fd, msg, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_sendmsg_zc
 */
template <FdLike Fd, typename FreeFunc>
inline auto async_sendmsg_zc(Fd fd, const struct msghdr *msg, unsigned flags,
                             FreeFunc &&func) {
    auto op = make_zero_copy_op_awaiter(
        std::forward<FreeFunc>(func), io_uring_prep_sendmsg_zc, fd, msg, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
/**
 * @brief See io_uring_prep_sendmsg_zc_fixed
 */
template <FdLike Fd, typename FreeFunc>
inline auto async_sendmsg_zc(Fd fd, detail::FixedBuffer<const msghdr *> msg,
                             unsigned flags, FreeFunc &&func) {
    auto op = make_zero_copy_op_awaiter(std::forward<FreeFunc>(func),
                                        io_uring_prep_sendmsg_zc_fixed, fd,
                                        msg.value, flags, msg.buf_index);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

/**
 * @brief See io_uring_prep_fsync
 */
template <FdLike Fd> inline auto async_fsync(Fd fd, unsigned fsync_flags) {
    auto op = make_op_awaiter(io_uring_prep_fsync, fd, fsync_flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_nop
 */
inline auto async_nop() { return make_op_awaiter(io_uring_prep_nop); }

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
/**
 * @brief See io_uring_prep_nop128
 */
inline auto async_nop128() { return make_op_awaiter128(io_uring_prep_nop128); }
#endif

/**
 * @brief See io_uring_prep_timeout
 */
inline auto async_timeout(struct __kernel_timespec *ts, unsigned count,
                          unsigned flags) {
    return make_op_awaiter(io_uring_prep_timeout, ts, count, flags);
}

#if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4

/**
 * @brief See io_uring_prep_timeout
 */
template <typename MultiShotFunc>
inline auto async_timeout_multishot(struct __kernel_timespec *ts,
                                    unsigned count, unsigned flags,
                                    MultiShotFunc &&func) {
    return make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
                                     io_uring_prep_timeout, ts, count,
                                     flags | IORING_TIMEOUT_MULTISHOT);
}
#endif

/**
 * @brief See io_uring_prep_accept
 */
template <FdLike Fd>
inline auto async_accept(Fd fd, struct sockaddr *addr, socklen_t *addrlen,
                         int flags) {
    auto op = make_op_awaiter(io_uring_prep_accept, fd, addr, addrlen, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_accept_direct
 */
template <FdLike Fd>
inline auto async_accept_direct(Fd fd, struct sockaddr *addr,
                                socklen_t *addrlen, int flags,
                                unsigned int file_index) {
    auto op = make_op_awaiter(io_uring_prep_accept_direct, fd, addr, addrlen,
                              flags, file_index);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_multishot_accept
 */
template <FdLike Fd, typename MultiShotFunc>
inline auto async_multishot_accept(Fd fd, struct sockaddr *addr,
                                   socklen_t *addrlen, int flags,
                                   MultiShotFunc &&func) {
    auto op = make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
                                        io_uring_prep_multishot_accept, fd,
                                        addr, addrlen, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_multishot_accept_direct
 */
template <FdLike Fd, typename MultiShotFunc>
inline auto async_multishot_accept_direct(Fd fd, struct sockaddr *addr,
                                          socklen_t *addrlen, int flags,
                                          MultiShotFunc &&func) {
    auto op = make_multishot_op_awaiter(std::forward<MultiShotFunc>(func),
                                        io_uring_prep_multishot_accept_direct,
                                        fd, addr, addrlen, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_cancel_fd
 */
template <FdLike Fd> inline auto async_cancel_fd(Fd fd, unsigned int flags) {
    if constexpr (detail::is_fixed_fd_v<Fd>) {
        flags |= IORING_ASYNC_CANCEL_FD_FIXED;
    }
    return make_op_awaiter(io_uring_prep_cancel_fd, fd, flags);
}

/**
 * @brief See io_uring_prep_link_timeout
 */
inline auto async_link_timeout(struct __kernel_timespec *ts, unsigned flags) {
    return make_op_awaiter(io_uring_prep_link_timeout, ts, flags);
}

/**
 * @brief See io_uring_prep_connect
 */
template <FdLike Fd>
inline auto async_connect(Fd fd, const struct sockaddr *addr,
                          socklen_t addrlen) {
    auto op = make_op_awaiter(io_uring_prep_connect, fd, addr, addrlen);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_files_update
 */
inline auto async_files_update(int *fds, unsigned nr_fds, int offset) {
    return make_op_awaiter(io_uring_prep_files_update, fds, nr_fds, offset);
}

/**
 * @brief See io_uring_prep_fallocate
 */
template <FdLike Fd>
inline auto async_fallocate(Fd fd, int mode, __u64 offset, __u64 len) {
    auto op = make_op_awaiter(io_uring_prep_fallocate, fd, mode, offset, len);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_openat
 */
inline auto async_openat(int dfd, const char *path, int flags, mode_t mode) {
    return make_op_awaiter(io_uring_prep_openat, dfd, path, flags, mode);
}

/**
 * @brief See io_uring_prep_openat_direct
 */
inline auto async_openat_direct(int dfd, const char *path, int flags,
                                mode_t mode, unsigned file_index) {
    return make_op_awaiter(io_uring_prep_openat_direct, dfd, path, flags, mode,
                           file_index);
}

/**
 * @brief See io_uring_prep_openat
 */
inline auto async_open(const char *path, int flags, mode_t mode) {
    return async_openat(AT_FDCWD, path, flags, mode);
}

/**
 * @brief See io_uring_prep_openat_direct
 */
inline auto async_open_direct(const char *path, int flags, mode_t mode,
                              unsigned file_index) {
    return async_openat_direct(AT_FDCWD, path, flags, mode, file_index);
}

/**
 * @brief See io_uring_prep_close
 */
inline auto async_close(int fd) {
    return make_op_awaiter(io_uring_prep_close, fd);
}

/**
 * @brief See io_uring_prep_close_direct
 */
inline auto async_close(detail::FixedFd fd) {
    return make_op_awaiter(io_uring_prep_close_direct, fd);
}

/**
 * @brief See io_uring_prep_read
 */
template <FdLike Fd, BufferLike Buffer>
inline auto async_read(Fd fd, Buffer &&buf, __u64 offset) {
    auto op =
        make_op_awaiter(io_uring_prep_read, fd, buf.data(), buf.size(), offset);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_read_fixed
 */
template <FdLike Fd, BufferLike Buffer>
inline auto async_read(Fd fd, detail::FixedBuffer<Buffer> buf, __u64 offset) {
    auto op = make_op_awaiter(io_uring_prep_read_fixed, fd, buf.value.data(),
                              buf.value.size(), offset, buf.buf_index);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_read
 */
template <FdLike Fd, NotBundledBufferRing Buffer>
inline auto async_read(Fd fd, Buffer &buf, __u64 offset) {
    auto op = make_select_buffer_op_awaiter(&buf, io_uring_prep_read, fd,
                                            nullptr, 0, offset);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
/**
 * @brief See io_uring_prep_read_multishot
 */
template <FdLike Fd, NotBundledBufferRing Buffer, typename MultiShotFunc>
inline auto async_read_multishot(Fd fd, Buffer &buf, __u64 offset,
                                 MultiShotFunc &&func) {
    auto op = make_multishot_select_buffer_op_awaiter(
        std::forward<MultiShotFunc>(func), &buf, io_uring_prep_read_multishot,
        fd, 0, offset, buf.bgid());
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

/**
 * @brief See io_uring_prep_write
 */
template <FdLike Fd, BufferLike Buffer>
inline auto async_write(Fd fd, Buffer &&buf, __u64 offset) {
    auto op = make_op_awaiter(io_uring_prep_write, fd, buf.data(), buf.size(),
                              offset);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_write_fixed
 */
template <FdLike Fd, BufferLike Buffer>
inline auto async_write(Fd fd, detail::FixedBuffer<Buffer> buf, __u64 offset) {
    auto op = make_op_awaiter(io_uring_prep_write_fixed, fd, buf.value.data(),
                              buf.value.size(), offset, buf.buf_index);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_statx
 */
inline auto async_statx(int dfd, const char *path, int flags, unsigned mask,
                        struct statx *statxbuf) {
    return make_op_awaiter(io_uring_prep_statx, dfd, path, flags, mask,
                           statxbuf);
}

/**
 * @brief See io_uring_prep_fadvise
 */
template <FdLike Fd>
inline auto async_fadvise(Fd fd, __u64 offset, off_t len, int advice) {
    auto op = make_op_awaiter(io_uring_prep_fadvise, fd, offset, len, advice);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @brief See io_uring_prep_fadvise64
 */
template <FdLike Fd>
inline auto async_fadvise64(Fd fd, __u64 offset, off_t len, int advice) {
    auto op = make_op_awaiter(io_uring_prep_fadvise64, fd, offset, len, advice);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

/**
 * @brief See io_uring_prep_madvise
 */
inline auto async_madvise(void *addr, __u32 length, int advice) {
    return make_op_awaiter(io_uring_prep_madvise, addr, length, advice);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @brief See io_uring_prep_madvise64
 */
inline auto async_madvise64(void *addr, off_t length, int advice) {
    auto op = make_op_awaiter(io_uring_prep_madvise64, addr, length, advice);
    return op;
}
#endif

namespace detail {

inline void prep_sendto(io_uring_sqe *sqe, int sockfd, const void *buf,
                        size_t len, int flags, const struct sockaddr *addr,
                        socklen_t addrlen) {
    io_uring_prep_send(sqe, sockfd, buf, len, flags);
    io_uring_prep_send_set_addr(sqe, addr, addrlen);
}

inline void prep_send_fixed(io_uring_sqe *sqe, int sockfd, const void *buf,
                            size_t len, int flags, int buf_index) {
    io_uring_prep_send(sqe, sockfd, buf, len, flags);
    sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
    sqe->buf_index = buf_index;
}

inline void prep_sendto_fixed(io_uring_sqe *sqe, int sockfd, const void *buf,
                              size_t len, int flags,
                              const struct sockaddr *addr, socklen_t addrlen,
                              int buf_index) {
    prep_sendto(sqe, sockfd, buf, len, flags, addr, addrlen);
    sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
    sqe->buf_index = buf_index;
}

inline void prep_sendto_zc(io_uring_sqe *sqe, int sockfd, const void *buf,
                           size_t len, int flags, const struct sockaddr *addr,
                           socklen_t addrlen, unsigned zc_flags) {
    io_uring_prep_send_zc(sqe, sockfd, buf, len, flags, zc_flags);
    io_uring_prep_send_set_addr(sqe, addr, addrlen);
}

inline void prep_sendto_zc_fixed(io_uring_sqe *sqe, int sockfd, const void *buf,
                                 size_t len, int flags,
                                 const struct sockaddr *addr, socklen_t addrlen,
                                 unsigned zc_flags, int buf_index) {
    prep_sendto_zc(sqe, sockfd, buf, len, flags, addr, addrlen, zc_flags);
    sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
    sqe->buf_index = buf_index;
}

} // namespace detail

/**
 * @brief See io_uring_prep_send
 */
template <FdLike Fd, BufferLike Buffer>
inline auto async_send(Fd sockfd, Buffer &&buf, int flags) {
    auto op = make_op_awaiter(io_uring_prep_send, sockfd, buf.data(),
                              buf.size(), flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

/**
 * @brief See io_uring_prep_send
 */
template <FdLike Fd>
inline auto async_send(Fd sockfd, ProvidedBufferQueue &buf, int flags) {
    auto op = make_select_buffer_op_awaiter(&buf, io_uring_prep_send, sockfd,
                                            nullptr, 0, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @brief See io_uring_prep_send
 */
template <FdLike Fd>
inline auto async_send(Fd sockfd, BundledProvidedBufferQueue &buf, int flags) {
    auto op = make_bundle_select_buffer_op_awaiter(&buf, io_uring_prep_send,
                                                   sockfd, nullptr, 0, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}
#endif

/**
 * @brief See io_uring_prep_send and io_uring_prep_send_set_addr
 */
template <FdLike Fd, BufferLike Buffer>
inline auto async_sendto(Fd sockfd, Buffer &&buf, int flags,
                         const struct sockaddr *addr, socklen_t addrlen) {
    auto op = make_op_awaiter(detail::prep_sendto, sockfd, buf.data(),
                              buf.size(), flags, addr, addrlen);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

/**
 * @brief See io_uring_prep_send and io_uring_prep_send_set_addr
 */
template <FdLike Fd>
inline auto async_sendto(Fd sockfd, ProvidedBufferQueue &buf, int flags,
                         const struct sockaddr *addr, socklen_t addrlen) {
    auto op = make_select_buffer_op_awaiter(&buf, detail::prep_sendto, sockfd,
                                            nullptr, 0, flags, addr, addrlen);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @brief See io_uring_prep_send and io_uring_prep_send_set_addr
 */
template <FdLike Fd>
inline auto async_sendto(Fd sockfd, BundledProvidedBufferQueue &buf, int flags,
                         const struct sockaddr *addr, socklen_t addrlen) {
    auto op = make_bundle_select_buffer_op_awaiter(
        &buf, detail::prep_sendto, sockfd, nullptr, 0, flags, addr, addrlen);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}
#endif

/**
 * @brief See io_uring_prep_send_zc
 */
template <FdLike Fd, typename Buffer, typename FreeFunc>
inline auto async_send_zc(Fd sockfd, Buffer &&buf, int flags, unsigned zc_flags,
                          FreeFunc &&func) {
    auto op = make_zero_copy_op_awaiter(
        std::forward<FreeFunc>(func), io_uring_prep_send_zc, sockfd, buf.data(),
        buf.size(), flags, zc_flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

/**
 * @brief See io_uring_prep_send_zc_fixed
 */
template <FdLike Fd, BufferLike Buffer, typename FreeFunc>
inline auto async_send_zc(Fd sockfd, detail::FixedBuffer<Buffer> buf, int flags,
                          unsigned zc_flags, FreeFunc &&func) {
    auto op = make_zero_copy_op_awaiter(
        std::forward<FreeFunc>(func), io_uring_prep_send_zc_fixed, sockfd,
        buf.value.data(), buf.value.size(), flags, zc_flags, buf.buf_index);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

/**
 * @brief See io_uring_prep_send_zc and io_uring_prep_send_set_addr
 */
template <FdLike Fd, BufferLike Buffer, typename FreeFunc>
inline auto async_sendto_zc(Fd sockfd, Buffer &&buf, int flags,
                            const struct sockaddr *addr, socklen_t addrlen,
                            unsigned zc_flags, FreeFunc &&func) {
    auto op = make_zero_copy_op_awaiter(
        std::forward<FreeFunc>(func), detail::prep_sendto_zc, sockfd,
        buf.data(), buf.size(), flags, addr, addrlen, zc_flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

/**
 * @brief See io_uring_prep_send_zc_fixed and io_uring_prep_send_set_addr
 */
template <FdLike Fd, BufferLike Buffer, typename FreeFunc>
inline auto async_sendto_zc(Fd sockfd, detail::FixedBuffer<Buffer> buf,
                            int flags, const struct sockaddr *addr,
                            socklen_t addrlen, unsigned zc_flags,
                            FreeFunc &&func) {
    auto op = make_zero_copy_op_awaiter(
        std::forward<FreeFunc>(func), detail::prep_sendto_zc_fixed, sockfd,
        buf.value.data(), buf.value.size(), flags, addr, addrlen, zc_flags,
        buf.buf_index);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

/**
 * @brief See io_uring_prep_recv
 */
template <FdLike Fd, BufferLike Buffer>
inline auto async_recv(Fd sockfd, Buffer &&buf, int flags) {
    auto op = make_op_awaiter(io_uring_prep_recv, sockfd, buf.data(),
                              buf.size(), flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

/**
 * @brief See io_uring_prep_recv
 */
template <FdLike Fd, NotBundledBufferRing Buffer>
inline auto async_recv(Fd sockfd, Buffer &buf, int flags) {
    auto op = make_select_buffer_op_awaiter(&buf, io_uring_prep_recv, sockfd,
                                            nullptr, 0, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @brief See io_uring_prep_recv
 */
template <FdLike Fd, BundledBufferRing Buffer>
inline auto async_recv(Fd sockfd, Buffer &buf, int flags) {
    auto op = make_bundle_select_buffer_op_awaiter(&buf, io_uring_prep_recv,
                                                   sockfd, nullptr, 0, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}
#endif

/**
 * @brief See io_uring_prep_recv_multishot
 */
template <FdLike Fd, NotBundledBufferRing Buffer, typename MultiShotFunc>
inline auto async_recv_multishot(Fd sockfd, Buffer &buf, int flags,
                                 MultiShotFunc &&func) {
    auto op = make_multishot_select_buffer_op_awaiter(
        std::forward<MultiShotFunc>(func), &buf, io_uring_prep_recv_multishot,
        sockfd, nullptr, 0, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @brief See io_uring_prep_recv_multishot
 */
template <FdLike Fd, BundledBufferRing Buffer, typename MultiShotFunc>
inline auto async_recv_multishot(Fd sockfd, Buffer &buf, int flags,
                                 MultiShotFunc &&func) {
    auto op = make_multishot_bundle_select_buffer_op_awaiter(
        std::forward<MultiShotFunc>(func), &buf, io_uring_prep_recv_multishot,
        sockfd, nullptr, 0, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), sockfd);
}
#endif

/**
 * @brief See io_uring_prep_openat2
 */
inline auto async_openat2(int dfd, const char *path, struct open_how *how) {
    return make_op_awaiter(io_uring_prep_openat2, dfd, path, how);
}

/**
 * @brief See io_uring_prep_openat2_direct
 */
inline auto async_openat2_direct(int dfd, const char *path,
                                 struct open_how *how, unsigned file_index) {
    return make_op_awaiter(io_uring_prep_openat2_direct, dfd, path, how,
                           file_index);
}

/**
 * @brief See io_uring_prep_shutdown
 */
template <FdLike Fd> inline auto async_shutdown(Fd fd, int how) {
    auto op = make_op_awaiter(io_uring_prep_shutdown, fd, how);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_unlinkat
 */
inline auto async_unlinkat(int dfd, const char *path, int flags) {
    return make_op_awaiter(io_uring_prep_unlinkat, dfd, path, flags);
}

/**
 * @brief See io_uring_prep_unlinkat
 */
inline auto async_unlink(const char *path, int flags) {
    return async_unlinkat(AT_FDCWD, path, flags);
}

/**
 * @brief See io_uring_prep_renameat
 */
inline auto async_renameat(int olddfd, const char *oldpath, int newdfd,
                           const char *newpath, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_renameat, olddfd, oldpath, newdfd,
                           newpath, flags);
}

/**
 * @brief See io_uring_prep_renameat
 */
inline auto async_rename(const char *oldpath, const char *newpath) {
    return async_renameat(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

/**
 * @brief See io_uring_prep_sync_file_range
 */
template <FdLike Fd>
inline auto async_sync_file_range(Fd fd, unsigned len, __u64 offset,
                                  int flags) {
    auto op =
        make_op_awaiter(io_uring_prep_sync_file_range, fd, len, offset, flags);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}

/**
 * @brief See io_uring_prep_mkdirat
 */
inline auto async_mkdirat(int dfd, const char *path, mode_t mode) {
    return make_op_awaiter(io_uring_prep_mkdirat, dfd, path, mode);
}

/**
 * @brief See io_uring_prep_mkdirat
 */
inline auto async_mkdir(const char *path, mode_t mode) {
    return async_mkdirat(AT_FDCWD, path, mode);
}

/**
 * @brief See io_uring_prep_symlinkat
 */
inline auto async_symlinkat(const char *target, int newdirfd,
                            const char *linkpath) {
    return make_op_awaiter(io_uring_prep_symlinkat, target, newdirfd, linkpath);
}

/**
 * @brief See io_uring_prep_symlinkat
 */
inline auto async_symlink(const char *target, const char *linkpath) {
    return async_symlinkat(target, AT_FDCWD, linkpath);
}

/**
 * @brief See io_uring_prep_linkat
 */
inline auto async_linkat(int olddfd, const char *oldpath, int newdfd,
                         const char *newpath, int flags) {
    return make_op_awaiter(io_uring_prep_linkat, olddfd, oldpath, newdfd,
                           newpath, flags);
}

/**
 * @brief See io_uring_prep_linkat
 */
inline auto async_link(const char *oldpath, const char *newpath, int flags) {
    return async_linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, flags);
}

/**
 * @brief See io_uring_prep_getxattr
 */
inline auto async_getxattr(const char *name, char *value, const char *path,
                           unsigned int len) {
    return make_op_awaiter(io_uring_prep_getxattr, name, value, path, len);
}

/**
 * @brief See io_uring_prep_setxattr
 */
inline auto async_setxattr(const char *name, const char *value,
                           const char *path, int flags, unsigned int len) {
    return make_op_awaiter(io_uring_prep_setxattr, name, value, path, flags,
                           len);
}

/**
 * @brief See io_uring_prep_fgetxattr
 */
inline auto async_fgetxattr(int fd, const char *name, char *value,
                            unsigned int len) {
    return make_op_awaiter(io_uring_prep_fgetxattr, fd, name, value, len);
}

/**
 * @brief See io_uring_prep_fsetxattr
 */
inline auto async_fsetxattr(int fd, const char *name, const char *value,
                            int flags, unsigned int len) {
    return make_op_awaiter(io_uring_prep_fsetxattr, fd, name, value, flags,
                           len);
}

/**
 * @brief See io_uring_prep_socket
 */
inline auto async_socket(int domain, int type, int protocol,
                         unsigned int flags) {
    return make_op_awaiter(io_uring_prep_socket, domain, type, protocol, flags);
}

/**
 * @brief See io_uring_prep_socket_direct
 */
inline auto async_socket_direct(int domain, int type, int protocol,
                                unsigned file_index, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_socket_direct, domain, type, protocol,
                           file_index, flags);
}

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
/**
 * @brief See io_uring_prep_uring_cmd
 */
template <FdLike Fd, typename CmdFunc>
inline auto async_uring_cmd(int cmd_op, Fd fd, CmdFunc &&cmd_func) {
    auto prep_func = [cmd_op, fd, cmd_func = std::forward<CmdFunc>(cmd_func)](
                         io_uring_sqe *sqe) {
        io_uring_prep_uring_cmd(sqe, cmd_op, fd);
        cmd_func(sqe);
    };
    auto op = make_op_awaiter(std::move(prep_func));
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
/**
 * @brief See io_uring_prep_cmd_sock
 */
template <FdLike Fd>
inline auto async_cmd_sock(int cmd_op, Fd fd, int level, int optname,
                           void *optval, int optlen) {
    auto op = make_op_awaiter(io_uring_prep_cmd_sock, cmd_op, fd, level,
                              optname, optval, optlen);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
/**
 * @brief See io_uring_prep_cmd_getsockname
 */
template <FdLike Fd>
inline auto async_cmd_getsockname(Fd fd, struct sockaddr *sockaddr,
                                  socklen_t *sockaddr_len, int peer) {
    auto op = make_op_awaiter(io_uring_prep_cmd_getsockname, fd, sockaddr,
                              sockaddr_len, peer);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
/**
 * @brief See io_uring_prep_waitid
 */
inline auto async_waitid(idtype_t idtype, id_t id, siginfo_t *infop,
                         int options, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_waitid, idtype, id, infop, options,
                           flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
/**
 * @brief See io_uring_prep_futex_wake
 */
inline auto async_futex_wake(uint32_t *futex, uint64_t val, uint64_t mask,
                             uint32_t futex_flags, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_futex_wake, futex, val, mask,
                           futex_flags, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
/**
 * @brief See io_uring_prep_futex_wait
 */
inline auto async_futex_wait(uint32_t *futex, uint64_t val, uint64_t mask,
                             uint32_t futex_flags, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_futex_wait, futex, val, mask,
                           futex_flags, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
/**
 * @brief See io_uring_prep_futex_waitv
 */
inline auto async_futex_waitv(struct futex_waitv *futex, uint32_t nr_futex,
                              unsigned int flags) {
    return make_op_awaiter(io_uring_prep_futex_waitv, futex, nr_futex, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
/**
 * @brief See io_uring_prep_fixed_fd_install
 */
inline auto async_fixed_fd_install(int fixed_fd, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_fixed_fd_install, fixed_fd, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
/**
 * @brief See io_uring_prep_msg_ring_fd
 */
inline auto async_fixed_fd_send(FdTable &dst, int source_fd, int target_fd,
                                unsigned int flags) {
    void *payload = nullptr;
    if (static_cast<unsigned int>(target_fd) != CONDY_FILE_INDEX_ALLOC) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        payload = reinterpret_cast<void *>((target_fd + 1) << 3);
    }
    return make_op_awaiter(
        io_uring_prep_msg_ring_fd, dst.ring_.ring_fd, source_fd, target_fd,
        reinterpret_cast<uint64_t>(encode_work(payload, WorkType::SendFd)),
        flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
/**
 * @brief See io_uring_prep_ftruncate
 */
template <FdLike Fd> inline auto async_ftruncate(Fd fd, loff_t len) {
    auto op = make_op_awaiter(io_uring_prep_ftruncate, fd, len);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 8) // >= 2.8
/**
 * @brief See io_uring_prep_cmd_discard
 */
template <FdLike Fd>
inline auto async_cmd_discard(Fd fd, uint64_t offset, uint64_t nbytes) {
    auto op = make_op_awaiter(io_uring_prep_cmd_discard, fd, offset, nbytes);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @brief See io_uring_prep_bind
 */
template <FdLike Fd>
inline auto async_bind(Fd fd, struct sockaddr *addr, socklen_t addrlen) {
    auto op = make_op_awaiter(io_uring_prep_bind, fd, addr, addrlen);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @brief See io_uring_prep_listen
 */
template <FdLike Fd> inline auto async_listen(Fd fd, int backlog) {
    auto op = make_op_awaiter(io_uring_prep_listen, fd, backlog);
    return detail::maybe_flag_fixed_fd(std::move(op), fd);
}
#endif

/**
 * @brief See io_uring_prep_epoll_ctl
 */
inline auto async_epoll_ctl(int epfd, int fd, int op, struct epoll_event *ev) {
    return make_op_awaiter(io_uring_prep_epoll_ctl, epfd, fd, op, ev);
}

#if !IO_URING_CHECK_VERSION(2, 10) // >= 2.10
/**
 * @brief See io_uring_prep_epoll_wait
 */
inline auto async_epoll_wait(int fd, struct epoll_event *events, int maxevents,
                             unsigned flags) {
    return make_op_awaiter(io_uring_prep_epoll_wait, fd, events, maxevents,
                           flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
/**
 * @brief See io_uring_prep_pipe
 */
inline auto async_pipe(int *fds, int pipe_flags) {
    return make_op_awaiter(io_uring_prep_pipe, fds, pipe_flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 12) // >= 2.12
/**
 * @brief See io_uring_prep_pipe_direct
 */
inline auto async_pipe_direct(int *fds, int pipe_flags,
                              unsigned int file_index) {
    return make_op_awaiter(io_uring_prep_pipe_direct, fds, pipe_flags,
                           file_index);
}
#endif

} // namespace condy
