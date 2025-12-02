#pragma once

#include "condy/awaiter_operations.hpp"
#include "condy/buffers.hpp"
#include "condy/condy_uring.hpp"
#include "condy/ring.hpp"
#include <liburing.h>
#include <type_traits>

#if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
#define CONDY_FILE_INDEX_ALLOC IORING_FILE_INDEX_ALLOC
#else
#define CONDY_FILE_INDEX_ALLOC (IORING_FILE_INDEX_ALLOC - 1)
#endif

namespace condy {

class ProvidedBufferPool {
public:
    ProvidedBufferPool(size_t log_num_buffers, size_t buffer_size,
                       unsigned int flags = 0)
        : impl_(std::make_shared<detail::ProvidedBufferPoolImpl>(
              Context::current().ring()->ring(),
              Context::current().runtime()->next_bgid(), log_num_buffers,
              buffer_size, flags)) {}

    ProvidedBufferPool(ProvidedBufferPool &&) = default;

    ProvidedBufferPool(const ProvidedBufferPool &) = delete;
    ProvidedBufferPool &operator=(const ProvidedBufferPool &) = delete;
    ProvidedBufferPool &operator=(ProvidedBufferPool &&) = delete;

public:
    detail::ProvidedBufferPoolImplPtr copy_impl() const & { return impl_; }
    detail::ProvidedBufferPoolImplPtr copy_impl() && {
        return std::move(impl_);
    }

private:
    detail::ProvidedBufferPoolImplPtr impl_;
};

// TODO: Need to test this
class ProvidedBufferQueue {
public:
    ProvidedBufferQueue(size_t log_num_buffers, unsigned int flags = 0)
        : impl_(std::make_shared<detail::ProvidedBufferQueueImpl>(
              Context::current().ring()->ring(),
              Context::current().runtime()->next_bgid(), log_num_buffers,
              flags)) {}

    ProvidedBufferQueue(ProvidedBufferQueue &&) = default;

    ProvidedBufferQueue(const ProvidedBufferQueue &) = delete;
    ProvidedBufferQueue &operator=(const ProvidedBufferQueue &) = delete;
    ProvidedBufferQueue &operator=(ProvidedBufferQueue &&) = delete;

public:
    detail::ProvidedBufferQueueImplPtr copy_impl() const & { return impl_; }
    detail::ProvidedBufferQueueImplPtr copy_impl() && {
        return std::move(impl_);
    }

private:
    detail::ProvidedBufferQueueImplPtr impl_;
};

namespace detail {

struct FixedFd {
    int value;
    operator int() const { return value; }
};

template <typename OpAwaiter>
void maybe_add_fixed_fd_flag(OpAwaiter &op, const FixedFd &) {
    op.add_flags(IOSQE_FIXED_FILE);
}

template <typename OpAwaiter>
void maybe_add_fixed_fd_flag(OpAwaiter &, int) { /* No-op */ }

template <typename Fd>
constexpr bool is_fixed_fd_v = std::is_same_v<std::decay_t<Fd>, FixedFd>;

template <typename ProvidedBuffer> class BundleProvidedBuffer {
public:
    BundleProvidedBuffer(ProvidedBuffer &buffer) : buffer_(buffer) {}

    ProvidedBuffer &get() & { return buffer_; }
    ProvidedBuffer get() && { return std::move(buffer_); }

private:
    ProvidedBuffer &buffer_;
};

template <typename Buffer>
struct is_bundle_provided_buffer : public std::false_type {};

template <typename Buffer>
struct is_bundle_provided_buffer<BundleProvidedBuffer<Buffer>>
    : public std::true_type {};

template <typename Buffer>
constexpr bool is_bundle_provided_buffer_v =
    is_bundle_provided_buffer<std::decay_t<Buffer>>::value;

template <typename Buffer>
constexpr bool is_provided_buffer_pool_v =
    std::is_same_v<std::decay_t<Buffer>, ProvidedBufferPool>;

template <typename Buffer>
constexpr bool is_provided_buffer_queue_v =
    std::is_same_v<std::decay_t<Buffer>, ProvidedBufferQueue>;

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

template <typename ProvidedBuffer> auto bundled(ProvidedBuffer &buffer) {
    return detail::BundleProvidedBuffer<std::decay_t<ProvidedBuffer>>(buffer);
}

template <typename Fd1, typename Fd2>
inline auto async_splice(Fd1 fd_in, int64_t off_in, Fd2 fd_out, int64_t off_out,
                         unsigned int nbytes, unsigned int splice_flags) {
    if constexpr (detail::is_fixed_fd_v<Fd1>) {
        splice_flags |= SPLICE_F_FD_IN_FIXED;
    }
    auto op = make_op_awaiter(io_uring_prep_splice, fd_in, off_in, fd_out,
                              off_out, nbytes, splice_flags);
    detail::maybe_add_fixed_fd_flag(op, fd_out);
    return op;
}

template <typename Fd1, typename Fd2>
inline auto async_tee(Fd1 fd_in, Fd2 fd_out, unsigned int nbytes,
                      unsigned int splice_flags) {
    if constexpr (detail::is_fixed_fd_v<Fd1>) {
        splice_flags |= SPLICE_F_FD_IN_FIXED;
    }
    auto op =
        make_op_awaiter(io_uring_prep_tee, fd_in, fd_out, nbytes, splice_flags);
    detail::maybe_add_fixed_fd_flag(op, fd_out);
    return op;
}

template <typename Fd>
inline auto async_readv(Fd fd, const struct iovec *iovecs, unsigned nr_vecs,
                        __u64 offset) {
    auto op = make_op_awaiter(io_uring_prep_readv, fd, iovecs, nr_vecs, offset);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

template <typename Fd>
inline auto async_readv2(Fd fd, const struct iovec *iovecs, unsigned nr_vecs,
                         __u64 offset, int flags) {
    auto op = make_op_awaiter(io_uring_prep_readv2, fd, iovecs, nr_vecs, offset,
                              flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

template <typename Fd>
inline auto async_writev(Fd fd, const struct iovec *iovecs, unsigned nr_vecs,
                         __u64 offset) {
    auto op =
        make_op_awaiter(io_uring_prep_writev, fd, iovecs, nr_vecs, offset);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

template <typename Fd>
inline auto async_writev2(Fd fd, const struct iovec *iovecs, unsigned nr_vecs,
                          __u64 offset, int flags) {
    auto op = make_op_awaiter(io_uring_prep_writev2, fd, iovecs, nr_vecs,
                              offset, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

template <typename Fd>
inline auto async_recvmsg(Fd fd, struct msghdr *msg, unsigned flags) {
    auto op = make_op_awaiter(io_uring_prep_recvmsg, fd, msg, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

template <typename Fd, typename MultiShotFunc, typename Buffer>
inline auto async_recvmsg_multishot(Fd fd, struct msghdr *msg, unsigned flags,
                                    Buffer &&buf_pool, MultiShotFunc &&func) {
    auto op = make_multishot_select_buffer_no_bundle_recv_op_awaiter(
        std::forward<MultiShotFunc>(func),
        std::forward<Buffer>(buf_pool).copy_impl(),
        io_uring_prep_recvmsg_multishot, fd, msg, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

template <typename Fd>
inline auto async_sendmsg(Fd fd, const struct msghdr *msg, unsigned flags) {
    auto op = make_op_awaiter(io_uring_prep_sendmsg, fd, msg, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

template <typename Fd> inline auto async_fsync(Fd fd, unsigned fsync_flags) {
    auto op = make_op_awaiter(io_uring_prep_fsync, fd, fsync_flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

inline auto async_nop() { return make_op_awaiter(io_uring_prep_nop); }

inline auto async_timeout(struct __kernel_timespec *ts, unsigned count,
                          unsigned flags) {
    return make_op_awaiter(io_uring_prep_timeout, ts, count, flags);
}

template <typename Fd>
inline auto async_accept(Fd fd, struct sockaddr *addr, socklen_t *addrlen,
                         int flags) {
    auto op = make_op_awaiter(io_uring_prep_accept, fd, addr, addrlen, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

template <typename Fd>
inline auto async_accept_direct(Fd fd, struct sockaddr *addr,
                                socklen_t *addrlen, int flags,
                                unsigned int file_index) {
    auto op = make_op_awaiter(io_uring_prep_accept_direct, fd, addr, addrlen,
                              flags, file_index);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

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

template <typename Fd> inline auto async_cancel_fd(Fd fd, unsigned int flags) {
    if constexpr (detail::is_fixed_fd_v<Fd>) {
        flags |= IORING_ASYNC_CANCEL_FD_FIXED;
    }
    return make_op_awaiter(io_uring_prep_cancel_fd, fd, flags);
}

inline auto async_link_timeout(struct __kernel_timespec *ts, unsigned flags) {
    return make_op_awaiter(io_uring_prep_link_timeout, ts, flags);
}

template <typename Fd>
inline auto async_connect(Fd fd, const struct sockaddr *addr,
                          socklen_t addrlen) {
    auto op = make_op_awaiter(io_uring_prep_connect, fd, addr, addrlen);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

inline auto FdTable::async_register_fd(int *fds, unsigned nr_fds, int offset) {
    return make_op_awaiter(io_uring_prep_files_update, fds, nr_fds, offset);
}

template <typename Fd>
inline auto async_fallocate(Fd fd, int mode, __u64 offset, __u64 len) {
    auto op = make_op_awaiter(io_uring_prep_fallocate, fd, mode, offset, len);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
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

inline auto async_close(detail::FixedFd fd) {
    return make_op_awaiter(io_uring_prep_close_direct, fd);
}

template <typename Fd, typename Buffer>
inline auto async_read(Fd fd, Buffer &&buf, __u64 offset) {
    auto op = [&] {
        if constexpr (detail::is_bundle_provided_buffer_v<Buffer>) {
            return make_select_buffer_recv_op_awaiter(
                std::forward<Buffer>(buf).get().copy_impl(), io_uring_prep_read,
                fd, nullptr, 0, offset);
        } else if constexpr (detail::is_provided_buffer_pool_v<Buffer>) {
            return make_select_buffer_no_bundle_recv_op_awaiter(
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
    auto op = [&] {
        if constexpr (detail::is_bundle_provided_buffer_v<Buffer>) {
            return make_multishot_select_buffer_recv_op_awaiter(
                std::forward<MultiShotFunc>(func),
                std::forward<Buffer>(buf).copy_impl(), prep, fd, offset);
        } else {
            return make_multishot_select_buffer_no_bundle_recv_op_awaiter(
                std::forward<MultiShotFunc>(func),
                std::forward<Buffer>(buf).copy_impl(), prep, fd, offset);
        }
    }();
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}
#endif

template <typename Fd, typename Buffer>
inline auto async_write(Fd fd, Buffer &&buf, __u64 offset) {
    auto op = [&] {
        if constexpr (detail::is_fixed_buffer_v<Buffer>) {
            return make_op_awaiter(io_uring_prep_write_fixed, fd, buf.data(),
                                   buf.size(), offset, buf.buf_index());
        } else if constexpr (detail::is_provided_buffer_queue_v<Buffer>) {
            return make_select_buffer_send_op_awaiter(
                std::forward<Buffer>(buf).copy_impl(), io_uring_prep_write, fd,
                nullptr, 0, offset);
        } else {
            return make_op_awaiter(io_uring_prep_write, fd, buf.data(),
                                   buf.size(), offset);
        }
    }();
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

inline auto async_statx(int dfd, const char *path, int flags, unsigned mask,
                        struct statx *statxbuf) {
    return make_op_awaiter(io_uring_prep_statx, dfd, path, flags, mask,
                           statxbuf);
}

template <typename Fd>
inline auto async_fadvise(Fd fd, __u64 offset, off_t len, int advice) {
    auto op = make_op_awaiter(io_uring_prep_fadvise, fd, offset, len, advice);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

inline auto async_madvise(void *addr, __u32 length, int advice) {
    return make_op_awaiter(io_uring_prep_madvise, addr, length, advice);
}

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

template <typename Fd, typename Buffer>
inline auto async_send(Fd sockfd, Buffer &&buf, int flags) {
    auto op = [&] {
        if constexpr (detail::is_fixed_buffer_v<Buffer>) {
            return make_op_awaiter(detail::prep_send_fixed, sockfd, buf.data(),
                                   buf.size(), flags, buf.buf_index());
        } else if constexpr (detail::is_provided_buffer_queue_v<Buffer>) {
            return make_select_buffer_send_op_awaiter(
                std::forward<Buffer>(buf).copy_impl(), io_uring_prep_send,
                sockfd, nullptr, 0, flags);
        } else {
            return make_op_awaiter(io_uring_prep_send, sockfd, buf.data(),
                                   buf.size(), flags);
        }
    }();
    detail::maybe_add_fixed_fd_flag(op, sockfd);
    return op;
}

template <typename Fd, typename Buffer>
inline auto async_sendto(Fd sockfd, Buffer &&buf, int flags,
                         const struct sockaddr *addr, socklen_t addrlen) {
    auto op = [&] {
        if constexpr (detail::is_fixed_buffer_v<Buffer>) {
            return make_op_awaiter(detail::prep_sendto_fixed, sockfd,
                                   buf.data(), buf.size(), flags, addr, addrlen,
                                   buf.buf_index());
        } else if constexpr (detail::is_provided_buffer_queue_v<Buffer>) {
            return make_select_buffer_send_op_awaiter(
                std::forward<Buffer>(buf).copy_impl(), detail::prep_sendto,
                sockfd, nullptr, 0, flags, addr, addrlen);
        } else {
            return make_op_awaiter(detail::prep_sendto, sockfd, buf.data(),
                                   buf.size(), flags, addr, addrlen);
        }
    }();
    detail::maybe_add_fixed_fd_flag(op, sockfd);
    return op;
}

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
    }();
    detail::maybe_add_fixed_fd_flag(op, sockfd);
    return op;
}

template <typename Fd, typename Buffer, typename FreeFunc>
inline auto async_sendto_zc(Fd sockfd, Buffer &&buf, int flags,
                            const struct sockaddr *addr, socklen_t addrlen,
                            unsigned zc_flags, FreeFunc &&func) {
    auto op = [&] {
        if constexpr (detail::is_fixed_buffer_v<Buffer>) {
            return make_zero_copy_op_awaiter(
                std::forward<FreeFunc>(func), detail::prep_sendto_zc_fixed,
                sockfd, buf.data(), buf.size(), flags, addr, addrlen, zc_flags,
                buf.buf_index());
        } else {
            return make_zero_copy_op_awaiter(
                std::forward<FreeFunc>(func), detail::prep_sendto_zc, sockfd,
                buf.data(), buf.size(), flags, addr, addrlen, zc_flags);
        }
    }();
    detail::maybe_add_fixed_fd_flag(op, sockfd);
    return op;
}

template <typename Fd, typename FreeFunc>
inline auto async_sendmsg_zc(Fd fd, const struct msghdr *msg, unsigned flags,
                             FreeFunc &&func) {
    auto op = make_zero_copy_op_awaiter(std::forward<FreeFunc>(func),
                                        io_uring_prep_sendmsg, fd, msg, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

namespace detail {

inline void prep_recv_fixed(io_uring_sqe *sqe, int sockfd, void *buf,
                            size_t len, int flags, int buf_index) {
    io_uring_prep_recv(sqe, sockfd, buf, len, flags);
    sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
    sqe->buf_index = buf_index;
}

} // namespace detail

template <typename Fd, typename Buffer>
inline auto async_recv(Fd sockfd, Buffer &&buf, int flags) {
    auto op = [&] {
        if constexpr (detail::is_bundle_provided_buffer_v<Buffer>) {
            return make_select_buffer_recv_op_awaiter(
                std::forward<Buffer>(buf).get().copy_impl(), io_uring_prep_recv,
                sockfd, nullptr, 0, flags);
        } else if constexpr (detail::is_provided_buffer_pool_v<Buffer>) {
            return make_select_buffer_no_bundle_recv_op_awaiter(
                std::forward<Buffer>(buf).copy_impl(), io_uring_prep_recv,
                sockfd, nullptr, 0, flags);
        } else if constexpr (detail::is_fixed_buffer_v<Buffer>) {
            return make_op_awaiter(detail::prep_recv_fixed, sockfd, buf.data(),
                                   buf.size(), flags, buf.buf_index());
        } else {
            return make_op_awaiter(io_uring_prep_recv, sockfd, buf.data(),
                                   buf.size(), flags);
        }
    }();
    detail::maybe_add_fixed_fd_flag(op, sockfd);
    return op;
}

template <typename Fd, typename Buffer, typename MultiShotFunc>
inline auto async_recv_multishot(Fd sockfd, Buffer &&buf, int flags,
                                 MultiShotFunc &&func) {
    auto op = [&] {
        if constexpr (detail::is_bundle_provided_buffer_v<Buffer>) {
            return make_multishot_select_buffer_recv_op_awaiter(
                std::forward<MultiShotFunc>(func),
                std::forward<Buffer>(buf).get().copy_impl(),
                io_uring_prep_recv_multishot, sockfd, nullptr, 0, flags);
        } else {
            return make_multishot_select_buffer_no_bundle_recv_op_awaiter(
                std::forward<MultiShotFunc>(func),
                std::forward<Buffer>(buf).copy_impl(),
                io_uring_prep_recv_multishot, sockfd, nullptr, 0, flags);
        }
    }();
    detail::maybe_add_fixed_fd_flag(op, sockfd);
    return op;
}

inline auto async_openat2(int dfd, const char *path, struct open_how *how) {
    return make_op_awaiter(io_uring_prep_openat2, dfd, path, how);
}

inline auto async_openat2_direct(int dfd, const char *path,
                                 struct open_how *how, unsigned file_index) {
    return make_op_awaiter(io_uring_prep_openat2_direct, dfd, path, how,
                           file_index);
}

template <typename Fd> inline auto async_shutdown(Fd fd, int how) {
    auto op = make_op_awaiter(io_uring_prep_shutdown, fd, how);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

inline auto async_unlinkat(int dfd, const char *path, int flags) {
    return make_op_awaiter(io_uring_prep_unlinkat, dfd, path, flags);
}

inline auto async_unlink(const char *path, int flags) {
    return make_op_awaiter(io_uring_prep_unlink, path, flags);
}

inline auto async_renameat(int olddfd, const char *oldpath, int newdfd,
                           const char *newpath, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_renameat, olddfd, oldpath, newdfd,
                           newpath, flags);
}

inline auto async_rename(const char *oldpath, const char *newpath) {
    return make_op_awaiter(io_uring_prep_rename, oldpath, newpath);
}

template <typename Fd>
inline auto async_sync_file_range(Fd fd, unsigned len, __u64 offset,
                                  int flags) {
    auto op =
        make_op_awaiter(io_uring_prep_sync_file_range, fd, len, offset, flags);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

inline auto async_mkdirat(int dfd, const char *path, mode_t mode) {
    return make_op_awaiter(io_uring_prep_mkdirat, dfd, path, mode);
}

inline auto async_mkdir(const char *path, mode_t mode) {
    return make_op_awaiter(io_uring_prep_mkdir, path, mode);
}

inline auto async_symlinkat(const char *target, int newdirfd,
                            const char *linkpath) {
    return make_op_awaiter(io_uring_prep_symlinkat, target, newdirfd, linkpath);
}

inline auto async_symlink(const char *target, const char *linkpath) {
    return make_op_awaiter(io_uring_prep_symlink, target, linkpath);
}

inline auto async_linkat(int olddfd, const char *oldpath, int newdfd,
                         const char *newpath, int flags) {
    return make_op_awaiter(io_uring_prep_linkat, olddfd, oldpath, newdfd,
                           newpath, flags);
}

inline auto async_link(const char *oldpath, const char *newpath, int flags) {
    return make_op_awaiter(io_uring_prep_link, oldpath, newpath, flags);
}

inline auto async_getxattr(const char *name, char *value, const char *path,
                           unsigned int len) {
    return make_op_awaiter(io_uring_prep_getxattr, name, value, path, len);
}

inline auto async_setxattr(const char *name, const char *value,
                           const char *path, int flags, unsigned int len) {
    return make_op_awaiter(io_uring_prep_setxattr, name, value, path, flags,
                           len);
}

template <typename Fd>
inline auto async_fgetxattr(Fd fd, const char *name, char *value,
                            unsigned int len) {
    auto op = make_op_awaiter(io_uring_prep_fgetxattr, fd, name, value, len);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

template <typename Fd>
inline auto async_fsetxattr(Fd fd, const char *name, const char *value,
                            int flags, unsigned int len) {
    auto op =
        make_op_awaiter(io_uring_prep_fsetxattr, fd, name, value, flags, len);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}

inline auto async_socket(int domain, int type, int protocol,
                         unsigned int flags) {
    return make_op_awaiter(io_uring_prep_socket, domain, type, protocol, flags);
}

inline auto async_socket_direct(int domain, int type, int protocol,
                                unsigned file_index, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_socket_direct, domain, type, protocol,
                           file_index, flags);
}

#if !IO_URING_CHECK_VERSION(2, 5) // >= 2.5
template <typename Fd>
inline auto async_cmd_sock(int cmd_op, Fd fd, int level, int optname,
                           void *optval, int optlen) {
    auto op = make_op_awaiter(io_uring_prep_cmd_sock, cmd_op, fd, level,
                              optname, optval, optlen);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
inline auto async_waitid(idtype_t idtype, id_t id, siginfo_t *infop,
                         int options, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_waitid, idtype, id, infop, options,
                           flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
inline auto async_futex_wake(uint32_t *futex, uint64_t val, uint64_t mask,
                             uint32_t futex_flags, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_futex_wake, futex, val, mask,
                           futex_flags, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
inline auto async_futex_wait(uint32_t *futex, uint64_t val, uint64_t mask,
                             uint32_t futex_flags, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_futex_wait, futex, val, mask,
                           futex_flags, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
inline auto async_futex_waitv(struct futex_waitv *futex, uint32_t nr_futex,
                              unsigned int flags) {
    return make_op_awaiter(io_uring_prep_futex_waitv, futex, nr_futex, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
inline auto FdTable::async_get_raw_fd(int fixed_fd, unsigned int flags) {
    return make_op_awaiter(io_uring_prep_fixed_fd_install, fixed_fd, flags);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 6) // >= 2.6
template <typename Fd> inline auto async_ftruncate(Fd fd, loff_t len) {
    auto op = make_op_awaiter(io_uring_prep_ftruncate, fd, len);
    detail::maybe_add_fixed_fd_flag(op, fd);
    return op;
}
#endif

} // namespace condy
