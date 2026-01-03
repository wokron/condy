/**
 * @file helpers.hpp
 * @brief Helper functions for asynchronous operations.
 * @details This file defines a set of helper functions primarily used in
 * conjunction with asynchronous operations to enhance their expressiveness and
 * usability.
 */

#pragma once

#include "condy/concepts.hpp"
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

/**
 * @brief Helper to build a function that spawns a coroutine on invocation.
 * @tparam CoroFunc Type of the coroutine function.
 * @param coro The coroutine function to be spawned.
 * @return A helper object that spawns the coroutine when invoked.
 */
template <typename CoroFunc> auto will_spawn(CoroFunc &&coro) {
    return detail::SpawnHelper<std::decay_t<CoroFunc>>{
        std::forward<CoroFunc>(coro)};
}

/**
 * @brief Helper to build a function that pushes to a channel on invocation.
 * @tparam Channel Type of the channel.
 * @param channel The channel to push to.
 * @return A helper object that pushes to the channel when invoked.
 */
template <typename Channel> auto will_push(Channel &channel) {
    return detail::PushHelper<std::decay_t<Channel>>{channel};
}

namespace detail {

struct FixedFd {
    int value;
    operator int() const { return value; }
};

template <typename T> struct FixedBuffer {
    T value;
    int buf_index;
};

} // namespace detail

inline auto fixed(int fd) { return detail::FixedFd{fd}; }

template <BufferLike Buffer> auto fixed(int buf_index, Buffer &&buf) {
    return detail::FixedBuffer<std::decay_t<Buffer>>{std::forward<Buffer>(buf),
                                                     buf_index};
}

inline auto fixed(int buf_index, const struct iovec *iov) {
    return detail::FixedBuffer<const iovec *>{iov, buf_index};
}

inline auto fixed(int buf_index, const struct msghdr *msg) {
    return detail::FixedBuffer<const msghdr *>{msg, buf_index};
}

inline auto &bundled(ProvidedBufferPool &buffer) {
    return static_cast<BundledProvidedBufferPool &>(buffer);
}
inline auto &bundled(ProvidedBufferQueue &buffer) {
    return static_cast<BundledProvidedBufferQueue &>(buffer);
}

} // namespace condy