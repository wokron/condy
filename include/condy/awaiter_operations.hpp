#pragma once

#include "condy/awaiters.hpp"
#include "condy/concepts.hpp"

namespace condy {

template <typename Func, typename... Args>
auto make_op_awaiter(Func &&func, Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
    };
    return OpAwaiter<decltype(prep_func)>(std::move(prep_func));
}

template <typename MultiShotFunc, typename Func, typename... Args>
auto make_multishot_op_awaiter(MultiShotFunc &&multishot_func, Func &&func,
                               Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
    };
    return MultiShotOpAwaiter<std::decay_t<MultiShotFunc>, decltype(prep_func)>(
        std::forward<MultiShotFunc>(multishot_func), std::move(prep_func));
}

template <BufferRingLike Br, typename Func, typename... Args>
auto make_select_buffer_op_awaiter(Br *buffers, Func &&func, Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
    };
    auto op = SelectBufferOpAwaiter<Br, decltype(prep_func)>(
        buffers, std::move(prep_func));
    return op;
}

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
    auto op = MultiShotSelectBufferOpAwaiter<std::decay_t<MultiShotFunc>, Br,
                                             decltype(prep_func)>(
        std::forward<MultiShotFunc>(multishot_func), buffers,
        std::move(prep_func));
    return op;
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
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
    auto op = SelectBufferOpAwaiter<Br, decltype(prep_func)>(
        buffers, std::move(prep_func));
    return op;
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
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
    auto op = MultiShotSelectBufferOpAwaiter<std::decay_t<MultiShotFunc>, Br,
                                             decltype(prep_func)>(
        std::forward<MultiShotFunc>(multishot_func), buffers,
        std::move(prep_func));
    return op;
}
#endif

template <typename FreeFunc, typename Func, typename... Args>
auto make_zero_copy_op_awaiter(FreeFunc &&free_func, Func &&func,
                               Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
    };
    return ZeroCopyOpAwaiter<std::decay_t<FreeFunc>, decltype(prep_func)>(
        std::forward<FreeFunc>(free_func), std::move(prep_func));
}

template <unsigned int Flags, AwaiterLike Awaiter>
auto flag(Awaiter &&awaiter) {
    return FlaggedOpAwaiter<Flags, std::decay_t<Awaiter>>(
        std::forward<Awaiter>(awaiter));
}

template <AwaiterLike Awaiter> auto drain(Awaiter &&awaiter) {
    return flag<IOSQE_IO_DRAIN>(std::forward<Awaiter>(awaiter));
}

template <AwaiterLike Awaiter> auto always_async(Awaiter &&awaiter) {
    return flag<IOSQE_ASYNC>(std::forward<Awaiter>(awaiter));
}

template <template <AwaiterLike... Awaiter> typename AwaiterType,
          AwaiterLike... Awaiter>
auto parallel(Awaiter &&...awaiters) {
    return AwaiterType<std::decay_t<Awaiter>...>(
        std::forward<Awaiter>(awaiters)...);
}

template <template <typename Awaiter> typename RangedAwaiterType,
          AwaiterRange Range>
auto parallel(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    auto begin = std::make_move_iterator(std::begin(range));
    auto end = std::make_move_iterator(std::end(range));
    std::vector<AwaiterType> awaiters(begin, end);
    return RangedAwaiterType<AwaiterType>(std::move(awaiters));
}

template <AwaiterLike... Awaiters> auto when_all(Awaiters &&...awaiters) {
    return parallel<WhenAllAwaiter>(std::forward<Awaiters>(awaiters)...);
}

template <AwaiterRange Range> auto when_all(Range &&range) {
    return parallel<RangedWhenAllAwaiter>(std::forward<Range>(range));
}

template <AwaiterLike... Awaiters> auto when_any(Awaiters &&...awaiters) {
    return parallel<WhenAnyAwaiter>(std::forward<Awaiters>(awaiters)...);
}

template <AwaiterRange Range> auto when_any(Range &&range) {
    return parallel<RangedWhenAnyAwaiter>(std::forward<Range>(range));
}

template <AwaiterLike... Awaiters> auto link(Awaiters &&...awaiters) {
    return parallel<LinkAwaiter>(std::forward<Awaiters>(awaiters)...);
}

template <AwaiterRange Range> auto link(Range &&range) {
    return parallel<RangedLinkAwaiter>(std::forward<Range>(range));
}

template <AwaiterLike... Awaiters> auto hard_link(Awaiters &&...awaiters) {
    return parallel<HardLinkAwaiter>(std::forward<Awaiters>(awaiters)...);
}

template <AwaiterRange Range> auto hard_link(Range &&range) {
    return parallel<RangedHardLinkAwaiter>(std::forward<Range>(range));
}

namespace operators {

template <AwaiterLike Awaiter1, AwaiterLike Awaiter2>
auto operator&&(Awaiter1 aw1, Awaiter2 aw2) {
    return when_all(std::move(aw1), std::move(aw2));
}

template <AwaiterLike Awaiter, AwaiterLike... Awaiters>
auto operator&&(WhenAllAwaiter<Awaiters...> aws, Awaiter aw) {
    return WhenAllAwaiter<Awaiters..., std::decay_t<Awaiter>>(std::move(aws),
                                                              std::move(aw));
}

template <AwaiterLike Awaiter1, AwaiterLike Awaiter2>
auto operator||(Awaiter1 aw1, Awaiter2 aw2) {
    return when_any(std::move(aw1), std::move(aw2));
}

template <AwaiterLike Awaiter, AwaiterLike... Awaiters>
auto operator||(WhenAnyAwaiter<Awaiters...> aws, Awaiter aw) {
    return WhenAnyAwaiter<Awaiters..., std::decay_t<Awaiter>>(std::move(aws),
                                                              std::move(aw));
}

template <AwaiterLike Awaiter1, AwaiterLike Awaiter2>
auto operator>>(Awaiter1 aw1, Awaiter2 aw2) {
    return link(std::move(aw1), std::move(aw2));
}

template <AwaiterLike Awaiter, AwaiterLike... Awaiters>
auto operator>>(LinkAwaiter<Awaiters...> aws, Awaiter aw) {
    return LinkAwaiter<Awaiters..., std::decay_t<Awaiter>>(std::move(aws),
                                                           std::move(aw));
}

} // namespace operators

} // namespace condy