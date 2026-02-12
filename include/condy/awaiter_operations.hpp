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

namespace condy {

/**
 * @brief This function creates a variant of OpAwaiter. OpAwaiter represents an
 * asynchronous operation that can be awaited. It is basically a wrapper around
 * an io_uring sqe preparation function.
 */
template <typename Func, typename... Args>
auto make_op_awaiter(Func &&func, Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
    };
    return OpAwaiter<decltype(prep_func), DefaultCQEHandler>(
        std::move(prep_func));
}

#if !IO_URING_CHECK_VERSION(2, 13) // >= 2.13
/**
 * @copydoc make_op_awaiter
 */
template <typename Func, typename... Args>
auto make_op_awaiter128(Func &&func, Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
    };
    return OpAwaiter<decltype(prep_func), DefaultCQEHandler, true>(
        std::move(prep_func));
}
#endif

/**
 * @copydoc make_op_awaiter
 */
template <typename MultiShotFunc, typename Func, typename... Args>
auto make_multishot_op_awaiter(MultiShotFunc &&multishot_func, Func &&func,
                               Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
    };
    return MultiShotOpAwaiter<decltype(prep_func), DefaultCQEHandler,
                              std::decay_t<MultiShotFunc>>(
        std::move(prep_func), std::forward<MultiShotFunc>(multishot_func));
}

/**
 * @copydoc make_op_awaiter
 */
template <BufferRingLike Br, typename Func, typename... Args>
auto make_select_buffer_op_awaiter(Br *buffers, Func &&func, Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
    };
    return OpAwaiter<decltype(prep_func), SelectBufferCQEHandler<Br>>(
        std::move(prep_func), buffers);
}

/**
 * @copydoc make_op_awaiter
 */
template <typename MultiShotFunc, BufferRingLike Br, typename Func,
          typename... Args>
auto make_multishot_select_buffer_op_awaiter(MultiShotFunc &&multishot_func,
                                             Br *buffers, Func &&func,
                                             Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
    };
    return MultiShotOpAwaiter<decltype(prep_func), SelectBufferCQEHandler<Br>,
                              MultiShotFunc>(
        std::move(prep_func), std::forward<MultiShotFunc>(multishot_func),
        buffers);
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @copydoc make_op_awaiter
 */
template <BufferRingLike Br, typename Func, typename... Args>
auto make_bundle_select_buffer_op_awaiter(Br *buffers, Func &&func,
                                          Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
        sqe->ioprio |= IORING_RECVSEND_BUNDLE;
    };
    return OpAwaiter<decltype(prep_func), SelectBufferCQEHandler<Br>>(
        std::move(prep_func), buffers);
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
/**
 * @copydoc make_op_awaiter
 */
template <typename MultiShotFunc, BufferRingLike Br, typename Func,
          typename... Args>
auto make_multishot_bundle_select_buffer_op_awaiter(
    MultiShotFunc &&multishot_func, Br *buffers, Func &&func, Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
        sqe->ioprio |= IORING_RECVSEND_BUNDLE;
    };
    return MultiShotOpAwaiter<decltype(prep_func), SelectBufferCQEHandler<Br>,
                              MultiShotFunc>(
        std::move(prep_func), std::forward<MultiShotFunc>(multishot_func),
        buffers);
}
#endif

/**
 * @copydoc make_op_awaiter
 */
template <typename FreeFunc, typename Func, typename... Args>
auto make_zero_copy_op_awaiter(FreeFunc &&free_func, Func &&func,
                               Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
    };
    return ZeroCopyOpAwaiter<decltype(prep_func), DefaultCQEHandler,
                             std::decay_t<FreeFunc>>(
        std::move(prep_func), std::forward<FreeFunc>(free_func));
}

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