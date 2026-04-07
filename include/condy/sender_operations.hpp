#pragma once

#include "condy/concepts.hpp"
#include "condy/cqe_handler.hpp"
#include "condy/senders.hpp"

namespace condy {

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename... Args>
auto build_op_sender(PrepFunc &&prep_func, Args &&...args) {
    return OpSender<PrepFunc, CQEHandler, Args...>(
        std::forward<PrepFunc>(prep_func), std::forward<Args>(args)...);
}

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc,
          typename MultiShotFunc, typename... Args>
auto build_multishot_op_sender(PrepFunc &&func, MultiShotFunc &&multishot_func,
                               Args &&...handler_args) {
    return MultiShotOpSender<std::decay_t<PrepFunc>, CQEHandler,
                             std::decay_t<MultiShotFunc>>(
        std::forward<PrepFunc>(func),
        std::forward<MultiShotFunc>(multishot_func),
        std::forward<Args>(handler_args)...);
}

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename FreeFunc,
          typename... Args>
auto build_zero_copy_op_sender(PrepFunc &&func, FreeFunc &&free_func,
                               Args &&...handler_args) {
    return ZeroCopyOpSender<std::decay_t<PrepFunc>, CQEHandler,
                            std::decay_t<FreeFunc>>(
        std::forward<PrepFunc>(func), std::forward<FreeFunc>(free_func),
        std::forward<Args>(handler_args)...);
}

namespace detail {

template <typename Func, typename... Args>
auto make_op_sender(Func &&func, Args &&...args) {
    auto prep_func = [func = std::forward<Func>(func),
                      ... args = std::forward<Args>(args)](Ring *ring) {
        auto *sqe = ring->get_sqe();
        func(sqe, args...);
        return sqe;
    };
    return build_op_sender<SimpleCQEHandler>(std::move(prep_func));
}

} // namespace detail

namespace detail {

template <typename Sender> class SenderAwaiter {
public:
    SenderAwaiter(Sender sender) : sender_(std::move(sender)) {}

    ~SenderAwaiter() { operation_state_.destroy(); }

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) noexcept {
        operation_state_.accept(
            [&] { return sender_.connect(Receiver{h, this}); });
        operation_state_.get().start(0);
    }

    auto await_resume() noexcept { return std::move(result_type); }

private:
    struct Receiver {
        std::coroutine_handle<> handle;
        SenderAwaiter *awaiter;

        template <typename... Args> void operator()(Sender::ReturnType result) {
            awaiter->result_type = std::move(result);
            handle.resume();
        }
    };

    using OperationState =
        decltype(std::declval<Sender>().connect(std::declval<Receiver>()));
    RawStorage<OperationState> operation_state_;
    Sender::ReturnType result_type;
    Sender sender_;
};

template <typename Sender> auto as_awaiter(Sender &&sender) {
    return detail::SenderAwaiter<std::decay_t<Sender>>(
        std::forward<Sender>(sender));
}

} // namespace detail

namespace temp {

template <typename... Senders> auto when_all(Senders &&...senders) {
    return WhenAllSender<std::decay_t<Senders>...>(
        std::forward<Senders>(senders)...);
}

template <typename... Senders> auto link(Senders &&...senders) {
    return LinkSender<std::decay_t<Senders>...>(
        std::forward<Senders>(senders)...);
}

template <typename... Senders> auto hard_link(Senders &&...senders) {
    return HardLinkSender<std::decay_t<Senders>...>(
        std::forward<Senders>(senders)...);
}

} // namespace temp

} // namespace condy