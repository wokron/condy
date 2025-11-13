#pragma once

#include "condy/awaiters.hpp"
#include "condy/task.hpp"

namespace condy {

template <typename Func, typename... Args>
auto make_op_awaiter(Func &&func, Args &&...args) {
    return OpAwaiter<std::decay_t<Func>, std::decay_t<Args>...>(
        std::forward<Func>(func), std::forward<Args>(args)...);
}

// Helper to spawn a coroutine from a multishot operation
template <typename CoroFunc> auto will_spawn(CoroFunc &&coro) {
    struct SpawnHelper {
        void operator()(int res) { co_spawn(func(res)).detach(); }
        std::decay_t<CoroFunc> func;
    };
    return SpawnHelper{std::forward<CoroFunc>(coro)};
}

template <typename MultiShotFunc, typename Func, typename... Args>
auto make_multishot_op_awaiter(MultiShotFunc &&multishot_func, Func &&func,
                               Args &&...args) {
    return MultiShotOpAwaiter<std::decay_t<MultiShotFunc>, std::decay_t<Func>,
                              std::decay_t<Args>...>(
        std::forward<MultiShotFunc>(multishot_func), std::forward<Func>(func),
        std::forward<Args>(args)...);
}

template <typename Func, typename... Args>
auto make_select_buffer_op_awaiter(detail::ProvidedBuffersImplPtr buffers_impl,
                                   Func &&func, Args &&...args) {
    int bgid = buffers_impl->bgid();
    auto op = SelectBufferOpAwaiter<std::decay_t<Func>, std::decay_t<Args>...>(
        std::move(buffers_impl), std::forward<Func>(func),
        std::forward<Args>(args)...);
    op.add_flags(IOSQE_BUFFER_SELECT);
    op.set_bgid(bgid);
    return op;
}

template <typename Func, typename... Args>
auto make_drained_op_awaiter(OpAwaiter<Func, Args...> op) {
    op.add_flags(IOSQE_IO_DRAIN);
    return op;
}

template <typename Condition, typename... Awaiters>
auto make_parallel_awaiter(Awaiters &&...awaiters) {
    return ParallelAwaiterWrapper<Condition, std::decay_t<Awaiters>...>(
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

template <typename Condition, typename Range>
auto make_ranged_parallel_awaiter(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    return RangedParallelAwaiterWrapper<Condition, AwaiterType>(
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

template <typename Func, typename... Args>
auto operator~(OpAwaiter<Func, Args...> op) {
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