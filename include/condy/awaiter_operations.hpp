#pragma once

#include "condy/awaiters.hpp"

namespace condy {

template <typename Func, typename... Args>
auto build_op_awaiter(Func &&func, Args &&...args) {
    return OpAwaiter<std::decay_t<Func>, std::decay_t<Args>...>(
        std::forward<Func>(func), std::forward<Args>(args)...);
}

template <typename... Awaiters> auto build_all_awaiter(Awaiters &&...awaiters) {
    return WaitAllAwaiter<std::decay_t<Awaiters>...>(
        std::forward<Awaiters>(awaiters)...);
}

template <typename... Awaiters> auto build_one_awaiter(Awaiters &&...awaiters) {
    return WaitOneAwaiter<std::decay_t<Awaiters>...>(
        std::forward<Awaiters>(awaiters)...);
}

template <typename... Awaiters>
auto build_link_awaiter(Awaiters &&...awaiters) {
    return LinkAwaiter<std::decay_t<Awaiters>...>(
        std::forward<Awaiters>(awaiters)...);
}

template <typename Range> auto build_ranged_all_awaiter(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    return RangedWaitAllAwaiter<AwaiterType>(std::forward<Range>(range));
}

template <typename Range> auto build_ranged_one_awaiter(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    return RangedWaitOneAwaiter<AwaiterType>(std::forward<Range>(range));
}

template <typename Range> auto build_ranged_link_awaiter(Range &&range) {
    using AwaiterType = typename std::decay_t<Range>::value_type;
    return RangedLinkAwaiter<AwaiterType>(std::forward<Range>(range));
}

template <typename Awaiter1, typename Awaiter2>
auto operator&&(Awaiter1 op1, Awaiter2 op2) {
    return build_all_awaiter(std::move(op1), std::move(op2));
}

template <typename Awaiter, typename... Awaiters>
auto operator&&(WaitAllAwaiter<Awaiters...> wa, Awaiter a) {
    return std::apply(
        [a = std::move(a)](auto &&...args) mutable {
            return build_all_awaiter(std::forward<decltype(args)>(args)...,
                                     std::move(a));
        },
        std::move(wa).awaiters());
}

template <typename Awaiter1, typename Awaiter2>
auto operator||(Awaiter1 op1, Awaiter2 op2) {
    return build_one_awaiter(std::move(op1), std::move(op2));
}

template <typename Awaiter, typename... Awaiters>
auto operator||(WaitOneAwaiter<Awaiters...> wa, Awaiter a) {
    return std::apply(
        [a = std::move(a)](auto &&...args) mutable {
            return build_one_awaiter(std::forward<decltype(args)>(args)...,
                                     std::move(a));
        },
        std::move(wa).awaiters());
}

template <typename Awaiter1, typename Awaiter2>
auto operator>>(Awaiter1 op1, Awaiter2 op2) {
    return build_link_awaiter(std::move(op1), std::move(op2));
}

template <typename Awaiter, typename... Awaiters>
auto operator>>(LinkAwaiter<Awaiters...> la, Awaiter a) {
    return std::apply(
        [a = std::move(a)](auto &&...args) mutable {
            return build_link_awaiter(std::forward<decltype(args)>(args)...,
                                      std::move(a));
        },
        std::move(la).awaiters());
}

template <typename Handle, typename Awaiter>
void operator+=(RangedParallelAwaiter<Handle, Awaiter> &rwa, Awaiter a) {
    rwa.append_awaiter(std::move(a));
}

} // namespace condy