#pragma once

#include "condy/concepts.hpp"
#include "condy/senders.hpp"
#include <coroutine>

namespace condy {

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename... Args>
auto build_op_sender(PrepFunc &&prep_func, Args &&...args) {
    return OpSender<std::decay_t<PrepFunc>, CQEHandler, std::decay_t<Args>...>(
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
    // Await/complete path is serialized, so atomic is not needed here.
    std::coroutine_handle<> handle_ = nullptr;
    OperationState operation_state_;
    typename Sender::ReturnType result_;
};

template <typename Sender> auto as_awaiter(Sender &&sender) {
    return detail::SenderAwaiter<std::decay_t<Sender>>(
        std::forward<Sender>(sender));
}

} // namespace detail

/**
 * @brief Decorates an operation with specific io_uring sqe flags.
 * @tparam Flags The io_uring sqe flags to set.
 * @param sender The operation to decorate.
 */
template <unsigned int Flags, typename Sender> auto flag(Sender &&sender) {
    return FlaggedOpSender<Flags, std::decay_t<Sender>>(
        std::forward<Sender>(sender));
}

/**
 * @brief Mark an operation as drain operation.
 * @param sender The operation to mark as drain.
 */
template <typename Sender> auto drain(Sender &&sender) {
    return flag<IOSQE_IO_DRAIN>(std::forward<Sender>(sender));
}

/**
 * @brief Mark an operation to always execute asynchronously.
 * @param sender The operation to mark as always async.
 */
template <typename Sender> auto always_async(Sender &&sender) {
    return flag<IOSQE_ASYNC>(std::forward<Sender>(sender));
}

/**
 * @brief Compose multiple operations into a single sender that executes them in
 * parallel.
 * @tparam SenderType The type of sender to compose into.
 * @param senders The operations to compose.
 */
template <template <typename... Senders> typename SenderType,
          typename... Senders>
auto parallel(Senders &&...senders) {
    return SenderType<std::decay_t<Senders>...>(
        std::forward<Senders>(senders)...);
}

/**
 * @brief Compose multiple operations from a range into a single operation that
 * executes them in parallel.
 * @tparam RangedSenderType The type of sender to compose into.
 * @param range The range of operations to compose.
 */
template <template <typename Sender> typename RangedSenderType,
          std::ranges::range Range>
auto parallel(Range &&range) {
    using SenderType = typename std::decay_t<Range>::value_type;
    auto begin = std::make_move_iterator(std::begin(range));
    auto end = std::make_move_iterator(std::end(range));
    std::vector<SenderType> senders(begin, end);
    return RangedSenderType<SenderType>(std::move(senders));
}

/**
 * @brief Compose multiple operations into a single operation that completes
 * when all of them complete.
 * @param senders The operations to compose.
 */
template <typename... Senders> auto when_all(Senders &&...senders) {
    return parallel<WhenAllSender>(std::forward<Senders>(senders)...);
}

/**
 * @brief Compose multiple operations from a range into a single operation that
 * completes when all of them complete.
 * @param range The range of operations to compose.
 */
template <std::ranges::range Range> auto when_all(Range &&range) {
    return parallel<RangedWhenAllSender>(std::forward<Range>(range));
}

/**
 * @brief Compose multiple operations into a single operation that completes
 * when any of them complete.
 * @param senders The operations to compose.
 */
template <typename... Senders> auto when_any(Senders &&...senders) {
    static_assert(sizeof...(Senders) > 0,
                  "when_any requires at least one sender");
    return parallel<WhenAnySender>(std::forward<Senders>(senders)...);
}

/**
 * @brief Compose multiple operations from a range into a single operation that
 * completes when any of them complete.
 * @param range The range of operations to compose.
 */
template <std::ranges::range Range> auto when_any(Range &&range) {
    return parallel<RangedWhenAnySender>(std::forward<Range>(range));
}

/**
 * @brief Compose multiple operations into a single operation that executes them
 * in sequence.
 * @param senders The operations to compose.
 */
template <typename... Senders> auto link(Senders &&...senders) {
    return parallel<LinkSender>(std::forward<Senders>(senders)...);
}

/**
 * @brief Compose multiple operations from a range into a single operation that
 * executes them in sequence.
 * @param range The range of operations to compose.
 */
template <std::ranges::range Range> auto link(Range &&range) {
    return parallel<RangedLinkSender>(std::forward<Range>(range));
}

/**
 * @brief Compose multiple operations into a single operation that executes them
 * in sequence and continues even if one of them fails.
 * @param senders The operations to compose.
 */
template <typename... Senders> auto hard_link(Senders &&...senders) {
    return parallel<HardLinkSender>(std::forward<Senders>(senders)...);
}

/**
 * @brief Compose multiple operations from a range into a single operation that
 * executes them in sequence and continues even if one of them fails.
 * @param range The range of operations to compose.
 */
template <std::ranges::range Range> auto hard_link(Range &&range) {
    return parallel<RangedHardLinkSender>(std::forward<Range>(range));
}

/**
 * @brief Operators for composing operations.
 */
namespace operators {

/**
 * @brief Operator overloads version of condy::when_all
 */
template <typename Sender1, typename Sender2>
auto operator&&(Sender1 s1, Sender2 s2) {
    return when_all(std::move(s1), std::move(s2));
}

/**
 * @brief Operator overloads version of condy::when_all
 */
template <typename S, typename... Ss>
auto operator&&(WhenAllSender<Ss...> aws, S sender) {
    return WhenAllSender<Ss..., std::decay_t<S>>(std::move(aws),
                                                 std::move(sender));
}

/**
 * @brief Operator overloads version of condy::when_any
 */
template <typename Sender1, typename Sender2>
auto operator||(Sender1 s1, Sender2 s2) {
    return when_any(std::move(s1), std::move(s2));
}

/**
 * @brief Operator overloads version of condy::when_any
 */
template <typename S, typename... Ss>
auto operator||(WhenAnySender<Ss...> aws, S sender) {
    return WhenAnySender<Ss..., std::decay_t<S>>(std::move(aws),
                                                 std::move(sender));
}

/**
 * @brief Operator overloads version of condy::link
 */
template <typename Sender1, typename Sender2>
auto operator>>(Sender1 s1, Sender2 s2) {
    return link(std::move(s1), std::move(s2));
}

/**
 * @brief Operator overloads version of condy::link
 */
template <typename S, typename... Ss>
auto operator>>(LinkSender<Ss...> aws, S sender) {
    return LinkSender<Ss..., std::decay_t<S>>(std::move(aws),
                                              std::move(sender));
}

} // namespace operators

} // namespace condy