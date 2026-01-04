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
/**
 * @brief Placeholder to let io_uring allocate a direct file descriptor.
 */
#define CONDY_FILE_INDEX_ALLOC IORING_FILE_INDEX_ALLOC
#else
/**
 * @brief Placeholder to let io_uring allocate a direct file descriptor.
 */
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
 * @brief Helper to build an invocable that spawns a coroutine on invocation.
 * @tparam CoroFunc Type of the coroutine function.
 * @param coro The coroutine function to be spawned.
 * @return An invocable object that spawns the coroutine when invoked.
 * @details The use case of this helper is to pass the invocable to an async
 * operation that accepts a completion callback. For example, @ref
 * async_multishot_accept().
 */
template <typename CoroFunc> auto will_spawn(CoroFunc &&coro) {
    return detail::SpawnHelper<std::decay_t<CoroFunc>>{
        std::forward<CoroFunc>(coro)};
}

/**
 * @brief Helper to build an invocable that pushes the result to a channel on
 * invocation.
 * @tparam Channel Type of the channel.
 * @param channel The channel to push to.
 * @return An invocable object that pushes to the channel when invoked.
 * @details The use case of this helper is to pass the invocable to an async
 * operation that accepts a completion callback. For example, @ref
 * async_read_multishot().
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

/**
 * @brief Mark a file descriptor as fixed for io_uring operations.
 * @details The async_* functions can identify the fixed file descriptor and set
 * the appropriate flags for io_uring operations.
 * @param fd The file descriptor to be marked as fixed. A fixed file
 * descriptor points to the index in the registered file descriptor table.
 * @return auto A helper object representing the fixed file descriptor.
 */
inline auto fixed(int fd) { return detail::FixedFd{fd}; }

/**
 * @brief Mark a buffer as fixed for io_uring operations.
 * @details The async_* functions can identify the fixed buffer and set the
 * appropriate flags for io_uring operations.
 * @tparam Buffer Type of the buffer.
 * @param buf The buffer to be marked as fixed.
 * @param buf_index The index in the registered buffer table.
 * @return auto A helper object representing the fixed buffer.
 */
template <BufferLike Buffer> auto fixed(int buf_index, Buffer &&buf) {
    return detail::FixedBuffer<std::decay_t<Buffer>>{std::forward<Buffer>(buf),
                                                     buf_index};
}

/**
 * @brief Mark iovecs as fixed for io_uring operations.
 * @details The async_* functions can identify the fixed iovec buffer and set
 * the appropriate flags for io_uring operations.
 * @param buf_index The index in the registered buffer table.
 * @param iov Pointer to the iovec array.
 * @return auto A helper object representing the fixed iovec buffer.
 */
inline auto fixed(int buf_index, const struct iovec *iov) {
    return detail::FixedBuffer<const iovec *>{iov, buf_index};
}

/**
 * @brief Mark msghdr as fixed for io_uring operations.
 * @details The async_* functions can identify the fixed msghdr buffer and set
 * the appropriate flags for io_uring operations.
 * @param buf_index The index in the registered buffer table.
 * @param msg Pointer to the msghdr structure.
 * @return auto A helper object representing the fixed msghdr buffer.
 */
inline auto fixed(int buf_index, const struct msghdr *msg) {
    return detail::FixedBuffer<const msghdr *>{msg, buf_index};
}

/**
 * @brief Get the bundled variant of a provided buffer pool. This will
 * enable buffer bundling feature of io_uring.
 * @param buffer The provided buffer pool.
 * @return auto& The bundled variant of the provided buffer.
 * @note When using bundled provided buffer pool, the return type of async
 * operations will be a vector of @ref ProvidedBuffer instead of a single
 * buffer.
 */
inline auto &bundled(ProvidedBufferPool &buffer) {
    return static_cast<BundledProvidedBufferPool &>(buffer);
}

/**
 * @brief Get the bundled variant of a provided buffer queue. This will
 * enable buffer bundling feature of io_uring.
 * @param buffer The provided buffer queue.
 * @return auto& The bundled variant of the provided buffer.
 */
inline auto &bundled(ProvidedBufferQueue &buffer) {
    return static_cast<BundledProvidedBufferQueue &>(buffer);
}

} // namespace condy