/**
 * @file awaiter_operations.hpp
 * @brief Helper functions for composing asynchronous operations.
 * @details This file provides a set of interfaces for constructing asynchronous
 * operations. It includes utilities for wrapping liburing interfaces as
 * awaitable asynchronous operations, as well as for composing and combining
 * these operations to enable collective execution.
 */

#pragma once

#include "condy/concepts.hpp"
#include "condy/cqe_handler.hpp"
#include "condy/ring.hpp"
#include "condy/sender_operations.hpp"

namespace condy {

/**
 * @brief Build a single-shot operation awaiter with custom CQE handler.
 * @tparam CQEHandler Type of CQE handler.
 * @tparam PrepFunc Type of preparation function.
 * @tparam Args Additional arguments for CQE handler construction.
 * @param func Preparation function that accepts `Ring*` and returns
 * `io_uring_sqe*`.
 * @param handler_args Arguments forwarded to CQE handler constructor.
 * @return OpAwaiter The constructed awaiter.
 */
template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename... Args>
auto build_op_awaiter(PrepFunc &&func, Args &&...handler_args) {
    return build_op_sender<CQEHandler>(std::forward<PrepFunc>(func),
                                       std::forward<Args>(handler_args)...);
}

/**
 * @brief Build a multi-shot operation awaiter with custom CQE handler.
 * @tparam CQEHandler Type of CQE handler.
 * @tparam PrepFunc Type of preparation function.
 * @tparam MultiShotFunc Type of callback function for multi-shot operations.
 * @tparam Args Additional arguments for CQE handler construction.
 * @param func Preparation function that accepts `Ring*` and returns
 * `io_uring_sqe*`.
 * @param multishot_func Callback invoked on each completion except the last
 * one.
 * @param handler_args Arguments forwarded to CQE handler constructor.
 * @return MultiShotOpAwaiter The constructed awaiter.
 */
template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc,
          typename MultiShotFunc, typename... Args>
auto build_multishot_op_awaiter(PrepFunc &&func, MultiShotFunc &&multishot_func,
                                Args &&...handler_args) {
    return build_multishot_op_sender<CQEHandler>(
        std::forward<PrepFunc>(func),
        std::forward<MultiShotFunc>(multishot_func),
        std::forward<Args>(handler_args)...);
}

/**
 * @brief Build a zero-copy operation awaiter with custom CQE handler.
 * @tparam CQEHandler Type of CQE handler.
 * @tparam PrepFunc Type of preparation function.
 * @tparam FreeFunc Type of resource cleanup function.
 * @tparam Args Additional arguments for CQE handler construction.
 * @param func Preparation function that accepts `Ring*` and returns
 * `io_uring_sqe*`.
 * @param free_func Cleanup function invoked when resource no longer needed.
 * @param handler_args Arguments forwarded to CQE handler constructor.
 * @return ZeroCopyOpAwaiter The constructed awaiter.
 */
template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename FreeFunc,
          typename... Args>
auto build_zero_copy_op_awaiter(PrepFunc &&func, FreeFunc &&free_func,
                                Args &&...handler_args) {
    return build_zero_copy_op_sender<CQEHandler>(
        std::forward<PrepFunc>(func), std::forward<FreeFunc>(free_func),
        std::forward<Args>(handler_args)...);
}

namespace detail {

template <typename Func, typename... Args>
auto make_op_awaiter(Func &&func, Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](Ring *ring) {
        auto *sqe = ring->get_sqe();
        func(sqe, args...);
        return sqe;
    };
    return build_op_awaiter<SimpleCQEHandler>(std::move(prep_func));
}

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
template <typename Func, typename... Args>
auto make_op_awaiter128(Func &&func, Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](Ring *ring) {
        auto *sqe = ring->get_sqe128();
        if (!sqe) {
            panic_on("SQE128 not enabled in the ring");
        }
        func(sqe, args...);
        return sqe;
    };
    return build_op_awaiter<SimpleCQEHandler>(std::move(prep_func));
}
#endif

template <typename MultiShotFunc, typename Func, typename... Args>
auto make_multishot_op_awaiter(MultiShotFunc &&multishot_func, Func &&func,
                               Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](Ring *ring) {
        auto *sqe = ring->get_sqe();
        func(sqe, args...);
        return sqe;
    };
    return build_multishot_op_awaiter<SimpleCQEHandler>(
        std::move(prep_func), std::forward<MultiShotFunc>(multishot_func));
}

template <BufferRingLike Br, typename Func, typename... Args>
auto make_select_buffer_op_awaiter(Br *buffers, Func &&func, Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](Ring *ring) {
        auto *sqe = ring->get_sqe();
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
        return sqe;
    };
    return build_op_awaiter<SelectBufferCQEHandler<Br>>(std::move(prep_func),
                                                        buffers);
}

template <typename MultiShotFunc, BufferRingLike Br, typename Func,
          typename... Args>
auto make_multishot_select_buffer_op_awaiter(MultiShotFunc &&multishot_func,
                                             Br *buffers, Func &&func,
                                             Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](Ring *ring) {
        auto *sqe = ring->get_sqe();
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
        return sqe;
    };
    return build_multishot_op_awaiter<SelectBufferCQEHandler<Br>>(
        std::move(prep_func), std::forward<MultiShotFunc>(multishot_func),
        buffers);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
template <BufferRingLike Br, typename Func, typename... Args>
auto make_bundle_select_buffer_op_awaiter(Br *buffers, Func &&func,
                                          Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](Ring *ring) {
        auto *sqe = ring->get_sqe();
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
        sqe->ioprio |= IORING_RECVSEND_BUNDLE;
        return sqe;
    };
    return build_op_awaiter<SelectBufferCQEHandler<Br>>(std::move(prep_func),
                                                        buffers);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
template <typename MultiShotFunc, BufferRingLike Br, typename Func,
          typename... Args>
auto make_multishot_bundle_select_buffer_op_awaiter(
    MultiShotFunc &&multishot_func, Br *buffers, Func &&func, Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](Ring *ring) {
        auto *sqe = ring->get_sqe();
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
        sqe->ioprio |= IORING_RECVSEND_BUNDLE;
        return sqe;
    };
    return build_multishot_op_awaiter<SelectBufferCQEHandler<Br>>(
        std::move(prep_func), std::forward<MultiShotFunc>(multishot_func),
        buffers);
}
#endif

template <typename FreeFunc, typename Func, typename... Args>
auto make_zero_copy_op_awaiter(FreeFunc &&free_func, Func &&func,
                               Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](Ring *ring) {
        auto *sqe = ring->get_sqe();
        func(sqe, args...);
        return sqe;
    };
    return build_zero_copy_op_awaiter<SimpleCQEHandler>(
        std::move(prep_func), std::forward<FreeFunc>(free_func));
}

} // namespace detail

} // namespace condy