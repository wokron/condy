#pragma once

#include "condy/concepts.hpp"
#include "condy/senders.hpp"
#include <coroutine>

namespace condy {

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename... Args>
auto build_op_sender(PrepFunc &&prep_func, Args &&...args) {
    return OpSender<PrepFunc, CQEHandler, std::decay_t<Args>...>(
        std::forward<PrepFunc>(prep_func), std::forward<Args>(args)...);
}

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc,
          typename MultiShotFunc, typename... Args>
auto build_multishot_op_sender(PrepFunc &&func, MultiShotFunc &&multishot_func,
                               Args &&...handler_args) {
    return MultiShotOpSender<std::decay_t<PrepFunc>, CQEHandler,
                             std::decay_t<MultiShotFunc>,
                             std::decay_t<Args>...>(
        std::forward<PrepFunc>(func),
        std::forward<MultiShotFunc>(multishot_func),
        std::forward<Args>(handler_args)...);
}

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename FreeFunc,
          typename... Args>
auto build_zero_copy_op_sender(PrepFunc &&func, FreeFunc &&free_func,
                               Args &&...handler_args) {
    return ZeroCopyOpSender<std::decay_t<PrepFunc>, CQEHandler,
                            std::decay_t<FreeFunc>, std::decay_t<Args>...>(
        std::forward<PrepFunc>(func), std::forward<FreeFunc>(free_func),
        std::forward<Args>(handler_args)...);
}

namespace detail {

template <typename Sender> class SenderAwaiter {
public:
    SenderAwaiter(Sender sender)
        : operation_state_(std::move(sender).connect(Receiver{this})) {}

    SenderAwaiter(const SenderAwaiter &) = delete;
    SenderAwaiter &operator=(const SenderAwaiter &) = delete;
    SenderAwaiter(SenderAwaiter &&) = delete;
    SenderAwaiter &operator=(SenderAwaiter &&) = delete;

public:
    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
        operation_state_.start(0);
        if (handle_ == std::noop_coroutine()) {
            // The operation completed synchronously, no need to suspend
            return false;
        } else {
            handle_ = h;
            return true;
        }
    }

    auto await_resume() noexcept { return std::move(result_); }

private:
    struct Receiver {
        SenderAwaiter *self;
        template <typename R> void operator()(R &&result) noexcept {
            self->handle_result_(std::forward<R>(result));
        }
        std::stop_token get_stop_token() const noexcept { return {}; }
    };

    template <typename R> void handle_result_(R &&result) {
        result_ = std::forward<R>(result);
        if (handle_) {
            handle_.resume();
        } else {
            handle_ = std::noop_coroutine();
        }
    }

    using OperationState =
        decltype(std::declval<Sender>().connect(std::declval<Receiver>()));
    std::coroutine_handle<> handle_ = nullptr;
    OperationState operation_state_;
    typename Sender::ReturnType result_;
};

template <typename Sender> auto as_awaiter(Sender &&sender) {
    return detail::SenderAwaiter<std::decay_t<Sender>>(
        std::forward<Sender>(sender));
}

} // namespace detail

template <unsigned int Flags, typename Sender> auto flag(Sender &&sender) {
    return FlaggedOpSender<Flags, std::decay_t<Sender>>(
        std::forward<Sender>(sender));
}

template <typename Sender> auto drain(Sender &&sender) {
    return flag<IOSQE_IO_DRAIN>(std::forward<Sender>(sender));
}

template <typename Sender> auto always_async(Sender &&sender) {
    return flag<IOSQE_ASYNC>(std::forward<Sender>(sender));
}

template <template <typename... Senders> typename SenderType,
          typename... Senders>
auto parallel(Senders &&...senders) {
    return SenderType<std::decay_t<Senders>...>(
        std::forward<Senders>(senders)...);
}

template <template <typename Sender> typename RangedSenderType,
          std::ranges::range Range>
auto parallel(Range &&range) {
    using SenderType = typename std::decay_t<Range>::value_type;
    auto begin = std::make_move_iterator(std::begin(range));
    auto end = std::make_move_iterator(std::end(range));
    std::vector<SenderType> senders(begin, end);
    return RangedSenderType<SenderType>(std::move(senders));
}

template <typename... Senders> auto when_all(Senders &&...senders) {
    return parallel<WhenAllSender>(std::forward<Senders>(senders)...);
}

template <std::ranges::range Range> auto when_all(Range &&range) {
    return parallel<RangedWhenAllSender>(std::forward<Range>(range));
}

template <typename... Senders> auto when_any(Senders &&...senders) {
    static_assert(sizeof...(Senders) > 0,
                  "when_any requires at least one sender");
    return parallel<WhenAnySender>(std::forward<Senders>(senders)...);
}

template <std::ranges::range Range> auto when_any(Range &&range) {
    return parallel<RangedWhenAnySender>(std::forward<Range>(range));
}

template <typename... Senders> auto link(Senders &&...senders) {
    return parallel<LinkSender>(std::forward<Senders>(senders)...);
}

template <std::ranges::range Range> auto link(Range &&range) {
    return parallel<RangedLinkSender>(std::forward<Range>(range));
}

template <typename... Senders> auto hard_link(Senders &&...senders) {
    return parallel<HardLinkSender>(std::forward<Senders>(senders)...);
}

template <std::ranges::range Range> auto hard_link(Range &&range) {
    return parallel<RangedHardLinkSender>(std::forward<Range>(range));
}

namespace operators {

template <typename Sender1, typename Sender2>
auto operator&&(Sender1 s1, Sender2 s2) {
    return when_all(std::move(s1), std::move(s2));
}

template <typename S, typename... Ss>
auto operator&&(WhenAllSender<Ss...> aws, S sender) {
    return WhenAllSender<Ss..., std::decay_t<S>>(std::move(aws),
                                                 std::move(sender));
}

template <typename Sender1, typename Sender2>
auto operator||(Sender1 s1, Sender2 s2) {
    return when_any(std::move(s1), std::move(s2));
}

template <typename S, typename... Ss>
auto operator||(WhenAnySender<Ss...> aws, S sender) {
    return WhenAnySender<Ss..., std::decay_t<S>>(std::move(aws),
                                                 std::move(sender));
}

template <typename Sender1, typename Sender2>
auto operator>>(Sender1 s1, Sender2 s2) {
    return link(std::move(s1), std::move(s2));
}

template <typename S, typename... Ss>
auto operator>>(LinkSender<Ss...> aws, S sender) {
    return LinkSender<Ss..., std::decay_t<S>>(std::move(aws),
                                              std::move(sender));
}

} // namespace operators

} // namespace condy