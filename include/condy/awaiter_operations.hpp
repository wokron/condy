#pragma once

#include "condy/awaiters.hpp"
#include "condy/task.hpp"

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

template <typename ProvidedBufferContainer, typename Func, typename... Args>
auto make_select_buffer_op_awaiter(ProvidedBufferContainer *buffers,
                                   Func &&func, Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
    };
    auto op =
        SelectBufferOpAwaiter<ProvidedBufferContainer, decltype(prep_func)>(
            buffers, std::move(prep_func));
    return op;
}

template <typename MultiShotFunc, typename ProvidedBufferContainer,
          typename Func, typename... Args>
auto make_multishot_select_buffer_op_awaiter(MultiShotFunc &&multishot_func,
                                             ProvidedBufferContainer *buffers,
                                             Func &&func, Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
    };
    auto op = MultiShotSelectBufferOpAwaiter<std::decay_t<MultiShotFunc>,
                                             ProvidedBufferContainer,
                                             decltype(prep_func)>(
        std::forward<MultiShotFunc>(multishot_func), buffers,
        std::move(prep_func));
    return op;
}

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
template <typename ProvidedBufferContainer, typename Func, typename... Args>
auto make_bundle_select_buffer_op_awaiter(ProvidedBufferContainer *buffers,
                                          Func &&func, Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
        sqe->ioprio |= IORING_RECVSEND_BUNDLE;
    };
    auto op =
        SelectBufferOpAwaiter<ProvidedBufferContainer, decltype(prep_func)>(
            buffers, std::move(prep_func));
    return op;
}
#endif

#if !IO_URING_CHECK_VERSION(2, 7) // >= 2.7
template <typename MultiShotFunc, typename ProvidedBufferContainer,
          typename Func, typename... Args>
auto make_multishot_bundle_select_buffer_op_awaiter(
    MultiShotFunc &&multishot_func, ProvidedBufferContainer *buffers,
    Func &&func, Args &&...args) {
    auto prep_func = [bgid = buffers->bgid(), func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](auto sqe) {
        func(sqe, args...);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
        sqe->ioprio |= IORING_RECVSEND_BUNDLE;
    };
    auto op = MultiShotSelectBufferOpAwaiter<std::decay_t<MultiShotFunc>,
                                             ProvidedBufferContainer,
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

template <typename Awaiter> auto make_drained_op_awaiter(Awaiter &&awaiter) {
    return DrainedOpAwaiter<std::decay_t<Awaiter>>(
        std::forward<Awaiter>(awaiter));
}

template <bool Cancel, typename... Awaiters>
auto make_parallel_awaiter(Awaiters &&...awaiters) {
    return ParallelAwaiterWrapper<Cancel, std::decay_t<Awaiters>...>(
        std::forward<Awaiters>(awaiters)...);
}

template <typename... Awaiters> auto make_all_awaiter(Awaiters &&...awaiters) {
    return WaitAllAwaiter<std::decay_t<Awaiters>...>(
        std::forward<Awaiters>(awaiters)...);
}

template <typename... Awaiters> auto make_one_awaiter(Awaiters &&...awaiters) {
    return WaitOneAwaiter<std::decay_t<Awaiters>...>(
        std::forward<Awaiters>(awaiters)...);
}

template <typename... Awaiters> auto make_link_awaiter(Awaiters &&...awaiters) {
    return LinkAwaiter<std::decay_t<Awaiters>...>(
        std::forward<Awaiters>(awaiters)...);
}

template <bool Cancel, typename Range>
auto make_ranged_parallel_awaiter(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    return RangedParallelAwaiterWrapper<Cancel, AwaiterType>(
        std::forward<Range>(range));
}

template <typename Range> auto make_ranged_all_awaiter(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    return RangedWaitAllAwaiter<AwaiterType>(std::forward<Range>(range));
}

template <typename Range> auto make_ranged_one_awaiter(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    return RangedWaitOneAwaiter<AwaiterType>(std::forward<Range>(range));
}

template <typename Range> auto make_ranged_link_awaiter(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    return RangedLinkAwaiter<AwaiterType>(std::forward<Range>(range));
}

namespace operators {

template <typename Func> auto operator~(OpAwaiter<Func> op) {
    return make_drained_op_awaiter(std::move(op));
}

template <typename Awaiter1, typename Awaiter2>
auto operator&&(Awaiter1 op1, Awaiter2 op2) {
    return make_all_awaiter(std::move(op1), std::move(op2));
}

template <typename Awaiter, typename... Awaiters>
auto operator&&(WaitAllAwaiter<Awaiters...> wa, Awaiter a) {
    return std::apply(
        [a = std::move(a)](auto &&...args) mutable {
            return make_all_awaiter(std::forward<decltype(args)>(args)...,
                                    std::move(a));
        },
        std::move(wa).awaiters());
}

template <typename Awaiter1, typename Awaiter2>
auto operator||(Awaiter1 op1, Awaiter2 op2) {
    return make_one_awaiter(std::move(op1), std::move(op2));
}

template <typename Awaiter, typename... Awaiters>
auto operator||(WaitOneAwaiter<Awaiters...> wa, Awaiter a) {
    return std::apply(
        [a = std::move(a)](auto &&...args) mutable {
            return make_one_awaiter(std::forward<decltype(args)>(args)...,
                                    std::move(a));
        },
        std::move(wa).awaiters());
}

template <typename Awaiter1, typename Awaiter2>
auto operator>>(Awaiter1 op1, Awaiter2 op2) {
    return make_link_awaiter(std::move(op1), std::move(op2));
}

template <typename Awaiter, typename... Awaiters>
auto operator>>(LinkAwaiter<Awaiters...> la, Awaiter a) {
    return std::apply(
        [a = std::move(a)](auto &&...args) mutable {
            return make_link_awaiter(std::forward<decltype(args)>(args)...,
                                     std::move(a));
        },
        std::move(la).awaiters());
}

template <typename Handle, typename Awaiter>
void operator+=(RangedParallelAwaiter<Handle, Awaiter> &rwa, Awaiter a) {
    rwa.append_awaiter(std::move(a));
}

} // namespace operators

} // namespace condy