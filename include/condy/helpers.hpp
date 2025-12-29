#pragma once

#include "condy/buffers.hpp"
#include "condy/condy_uring.hpp"
#include "condy/provided_buffers.hpp"
#include <type_traits>

#if !IO_URING_CHECK_VERSION(2, 4) // >= 2.4
#define CONDY_FILE_INDEX_ALLOC IORING_FILE_INDEX_ALLOC
#else
#define CONDY_FILE_INDEX_ALLOC (IORING_FILE_INDEX_ALLOC - 1)
#endif

namespace condy {

namespace detail {

template <typename CoroFunc> struct SpawnHelper {
    void operator()(auto &&res) {
        co_spawn(func(std::forward<decltype(res)>(res))).detach();
    }
    std::decay_t<CoroFunc> func;
};

template <typename Channel> struct PushHelper {
    void operator()(auto &&res) {
        channel.force_push(std::forward<decltype(res)>(res));
    }
    std::decay_t<Channel> &channel;
};

} // namespace detail

// Helper to spawn a coroutine from a multishot operation
template <typename CoroFunc> auto will_spawn(CoroFunc &&coro) {
    return detail::SpawnHelper<std::decay_t<CoroFunc>>{
        std::forward<CoroFunc>(coro)};
}

// Helper to push result to the channel
template <typename Channel> auto will_push(Channel &channel) {
    return detail::PushHelper<std::decay_t<Channel>>{channel};
}

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

template <typename Buffer>
constexpr bool is_provided_buffer_pool_v =
    std::is_same_v<Buffer, ProvidedBufferPool &>;

template <typename Buffer>
constexpr bool is_bundled_provided_buffer_pool_v =
    std::is_same_v<Buffer, BundledProvidedBufferPool &>;

template <typename Buffer>
constexpr bool is_provided_buffer_queue_v =
    std::is_same_v<Buffer, ProvidedBufferQueue &>;

template <typename Buffer>
constexpr bool is_bundled_provided_buffer_queue_v =
    std::is_same_v<Buffer, BundledProvidedBufferQueue &>;

template <typename BufferBase> class FixedBuffer : public BufferBase {
public:
    FixedBuffer(int buf_index, BufferBase base)
        : BufferBase(base), buf_index_(buf_index) {}

    int buf_index() const { return buf_index_; }

private:
    int buf_index_;
};

template <typename Buffer>
constexpr bool is_basic_buffer =
    std::is_same_v<std::decay_t<Buffer>, MutableBuffer> ||
    std::is_same_v<std::decay_t<Buffer>, ConstBuffer>;

template <typename Buffer> struct is_fixed_buffer : public std::false_type {};
template <typename BufferBase>
struct is_fixed_buffer<FixedBuffer<BufferBase>> : public std::true_type {};
template <typename Buffer>
constexpr bool is_fixed_buffer_v = is_fixed_buffer<std::decay_t<Buffer>>::value;

struct FixedIoVec {
    int buf_index;
    iovec *iov;
};

template <typename IoVec> struct is_fixed_iovec : public std::false_type {};
template <> struct is_fixed_iovec<FixedIoVec> : public std::true_type {};
template <typename IoVec>
constexpr bool is_fixed_iovec_v = is_fixed_iovec<std::decay_t<IoVec>>::value;

struct FixedMsghdr {
    int buf_index;
    msghdr *msg;
};

template <typename MsgHdr> struct is_fixed_msghdr : public std::false_type {};
template <> struct is_fixed_msghdr<FixedMsghdr> : public std::true_type {};
template <typename MsgHdr>
constexpr bool is_fixed_msghdr_v = is_fixed_msghdr<std::decay_t<MsgHdr>>::value;

} // namespace detail

// Helper to specify a fixed fd
inline auto fixed(int fd) { return detail::FixedFd{fd}; }

// Helper to specify a fixed buffer
template <typename Buffer>
    requires(detail::is_basic_buffer<Buffer>)
auto fixed(int buf_index, Buffer &&buf) {
    return detail::FixedBuffer<std::decay_t<Buffer>>(buf_index,
                                                     std::forward<Buffer>(buf));
}

// Helper to specify a fixed iovec
inline auto fixed(int buf_index, iovec *iov) {
    return detail::FixedIoVec{buf_index, iov};
}

// Helper to specify a fixed msghdr
inline auto fixed(int buf_index, struct msghdr *msg) {
    return detail::FixedMsghdr{buf_index, msg};
}

// Helper to bundle provided buffers
inline auto &bundled(ProvidedBufferPool &buffer) {
    return static_cast<BundledProvidedBufferPool &>(buffer);
}
inline auto &bundled(ProvidedBufferQueue &buffer) {
    return static_cast<BundledProvidedBufferQueue &>(buffer);
}

} // namespace condy