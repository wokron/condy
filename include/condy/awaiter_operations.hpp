/**
 * @file awaiter_operations.hpp
 * @brief Helper functions for composing asynchronous operations.
 * @details This file provides a set of interfaces for constructing asynchronous
 * operations. It includes utilities for wrapping liburing interfaces as
 * awaitable asynchronous operations, as well as for composing and combining
 * these operations to enable collective execution.
 */

#pragma once

#include "condy/awaiters.hpp"
#include "condy/concepts.hpp"
#include "condy/cqe_handler.hpp"
#include "condy/ring.hpp"

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
    return OpAwaiter<std::decay_t<PrepFunc>, CQEHandler>(
        std::forward<PrepFunc>(func), std::forward<Args>(handler_args)...);
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
    return MultiShotOpAwaiter<std::decay_t<PrepFunc>, CQEHandler,
                              std::decay_t<MultiShotFunc>>(
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
    return ZeroCopyOpAwaiter<std::decay_t<PrepFunc>, CQEHandler,
                             std::decay_t<FreeFunc>>(
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

/**
 * @brief Decorates an awaiter with specific io_uring sqe flags.
 * @tparam Flags The io_uring sqe flags to set.
 * @param awaiter The awaiter to decorate.
 * @return auto The decorated awaiter.
 */
template <unsigned int Flags, AwaiterLike Awaiter>
auto flag(Awaiter &&awaiter) {
    return FlaggedOpAwaiter<Flags, std::decay_t<Awaiter>>(
        std::forward<Awaiter>(awaiter));
}

/**
 * @brief Mark an awaiter as drain operation.
 * @param awaiter The awaiter to decorate.
 */
template <AwaiterLike Awaiter> auto drain(Awaiter &&awaiter) {
    return flag<IOSQE_IO_DRAIN>(std::forward<Awaiter>(awaiter));
}

/**
 * @brief Mark an awaiter to always execute asynchronously.
 * @param awaiter The awaiter to decorate.
 */
template <AwaiterLike Awaiter> auto always_async(Awaiter &&awaiter) {
    return flag<IOSQE_ASYNC>(std::forward<Awaiter>(awaiter));
}

/**
 * @brief Compose multiple awaiters into a single awaiter that executes them in
 * parallel.
 * @tparam AwaiterType Awaiter template to use for composing the awaiters.
 * @tparam Awaiter Types of the awaiters to compose.
 * @param awaiters The awaiters to compose.
 * @return auto The composed awaiter.
 */
template <template <AwaiterLike... Awaiter> typename AwaiterType,
          AwaiterLike... Awaiter>
auto parallel(Awaiter &&...awaiters) {
    return AwaiterType<std::decay_t<Awaiter>...>(
        std::forward<Awaiter>(awaiters)...);
}

/**
 * @brief Compose multiple awaiters from a range into a single awaiter that
 * executes them in parallel.
 * @tparam RangedAwaiterType Awaiter template to use for composing the awaiters.
 * @tparam Range Type of the range containing the awaiters.
 * @param range The range of awaiters to compose.
 * @return auto The composed awaiter.
 */
template <template <typename Awaiter> typename RangedAwaiterType,
          AwaiterRange Range>
auto parallel(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    auto begin = std::make_move_iterator(std::begin(range));
    auto end = std::make_move_iterator(std::end(range));
    std::vector<AwaiterType> awaiters(begin, end);
    return RangedAwaiterType<AwaiterType>(std::move(awaiters));
}

/**
 * @brief Compose multiple awaiters into a single awaiter that completes when
 * all of them complete.
 * @tparam Awaiters Types of the awaiters to compose.
 * @param awaiters The awaiters to compose.
 * @return WhenAllAwaiter The composed awaiter.
 */
template <AwaiterLike... Awaiters> auto when_all(Awaiters &&...awaiters) {
    return parallel<WhenAllAwaiter>(std::forward<Awaiters>(awaiters)...);
}

/**
 * @brief Compose multiple awaiters from a range into a single awaiter that
 * completes when all of them complete.
 * @tparam Range Type of the range containing the awaiters.
 * @param range The range of awaiters to compose.
 * @return RangedWhenAllAwaiter The composed awaiter.
 */
template <AwaiterRange Range> auto when_all(Range &&range) {
    return parallel<RangedWhenAllAwaiter>(std::forward<Range>(range));
}

/**
 * @brief Compose multiple awaiters into a single awaiter that completes when
 * any of them complete.
 * @tparam Awaiters Types of the awaiters to compose.
 * @param awaiters The awaiters to compose.
 * @return WhenAnyAwaiter The composed awaiter.
 * @note If multiple awaiters complete simultaneously, the result will only
 * contain one of the results.
 */
template <AwaiterLike... Awaiters> auto when_any(Awaiters &&...awaiters) {
    return parallel<WhenAnyAwaiter>(std::forward<Awaiters>(awaiters)...);
}

/**
 * @brief Compose multiple awaiters from a range into a single awaiter that
 * completes when any of them complete.
 * @tparam Range Type of the range containing the awaiters.
 * @param range The range of awaiters to compose.
 * @return RangedWhenAnyAwaiter The composed awaiter.
 */
template <AwaiterRange Range> auto when_any(Range &&range) {
    return parallel<RangedWhenAnyAwaiter>(std::forward<Range>(range));
}

/**
 * @brief Compose multiple awaiters into a single awaiter that executes them in
 * sequence.
 * @tparam Awaiters Types of the awaiters to compose.
 * @param awaiters The awaiters to compose.
 * @return LinkAwaiter The composed awaiter.
 */
template <AwaiterLike... Awaiters> auto link(Awaiters &&...awaiters) {
    return parallel<LinkAwaiter>(std::forward<Awaiters>(awaiters)...);
}

/**
 * @brief Compose multiple awaiters from a range into a single awaiter that
 * executes them in sequence.
 * @tparam Range Type of the range containing the awaiters.
 * @param range The range of awaiters to compose.
 * @return RangedLinkAwaiter The composed awaiter.
 */
template <AwaiterRange Range> auto link(Range &&range) {
    return parallel<RangedLinkAwaiter>(std::forward<Range>(range));
}

/**
 * @brief Compose multiple awaiters into a single awaiter that executes them in
 * sequence and continues even if one of them fails.
 * @tparam Awaiters Types of the awaiters to compose.
 * @param awaiters The awaiters to compose.
 * @return HardLinkAwaiter The composed awaiter.
 */
template <AwaiterLike... Awaiters> auto hard_link(Awaiters &&...awaiters) {
    return parallel<HardLinkAwaiter>(std::forward<Awaiters>(awaiters)...);
}

/**
 * @brief Compose multiple awaiters from a range into a single awaiter that
 * executes them in sequence and continues even if one of them fails.
 * @tparam Range Type of the range containing the awaiters.
 * @param range The range of awaiters to compose.
 * @return RangedHardLinkAwaiter The composed awaiter.
 */
template <AwaiterRange Range> auto hard_link(Range &&range) {
    return parallel<RangedHardLinkAwaiter>(std::forward<Range>(range));
}

/**
 * @brief Operators for composing awaiters.
 */
namespace operators {

/**
 * @brief Operator overloads version of when_all
 */
template <AwaiterLike Awaiter1, AwaiterLike Awaiter2>
auto operator&&(Awaiter1 aw1, Awaiter2 aw2) {
    return when_all(std::move(aw1), std::move(aw2));
}

/**
 * @brief Operator overloads version of when_all
 */
template <AwaiterLike Awaiter, AwaiterLike... Awaiters>
auto operator&&(WhenAllAwaiter<Awaiters...> aws, Awaiter aw) {
    return WhenAllAwaiter<Awaiters..., std::decay_t<Awaiter>>(std::move(aws),
                                                              std::move(aw));
}

/**
 * @brief Operator overloads version of when_any
 */
template <AwaiterLike Awaiter1, AwaiterLike Awaiter2>
auto operator||(Awaiter1 aw1, Awaiter2 aw2) {
    return when_any(std::move(aw1), std::move(aw2));
}

/**
 * @brief Operator overloads version of when_any
 */
template <AwaiterLike Awaiter, AwaiterLike... Awaiters>
auto operator||(WhenAnyAwaiter<Awaiters...> aws, Awaiter aw) {
    return WhenAnyAwaiter<Awaiters..., std::decay_t<Awaiter>>(std::move(aws),
                                                              std::move(aw));
}

/**
 * @brief Operator overloads version of link
 */
template <AwaiterLike Awaiter1, AwaiterLike Awaiter2>
auto operator>>(Awaiter1 aw1, Awaiter2 aw2) {
    return link(std::move(aw1), std::move(aw2));
}

/**
 * @brief Operator overloads version of link
 */
template <AwaiterLike Awaiter, AwaiterLike... Awaiters>
auto operator>>(LinkAwaiter<Awaiters...> aws, Awaiter aw) {
    return LinkAwaiter<Awaiters..., std::decay_t<Awaiter>>(std::move(aws),
                                                           std::move(aw));
}

} // namespace operators

} // namespace condy