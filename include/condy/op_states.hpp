/**
 * @file op_states.hpp
 * @brief Operation state implementations for various operations.
 */

#pragma once

#include "condy/concepts.hpp"
#include "condy/condy_uring.hpp"
#include "condy/finish_handles.hpp"
#include "condy/type_traits.hpp"
#include "condy/utils.hpp"
#include <array>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <tuple>
#include <vector>

namespace condy {
namespace detail {

template <typename Handle, PrepFuncLike Func> class OpSenderOperationState {
public:
    template <typename... HandleArgs>
    OpSenderOperationState(Func prep_func, HandleArgs &&...handle_args)
        : prep_func_(std::move(prep_func)),
          finish_handle_(std::forward<HandleArgs>(handle_args)...) {}

    OpSenderOperationState(OpSenderOperationState &&) = delete;
    OpSenderOperationState &operator=(OpSenderOperationState &&) = delete;
    OpSenderOperationState(const OpSenderOperationState &) = delete;
    OpSenderOperationState &operator=(const OpSenderOperationState &) = delete;

public:
    void start(unsigned int flags) noexcept {
        auto &context = detail::Context::current();
        auto *ring = context.ring();
        context.runtime()->pend_work();
        io_uring_sqe *sqe = prep_func_(ring);
        assert(sqe && "prep_func must return a valid sqe");
        io_uring_sqe_set_flags(sqe, sqe->flags | flags);
        auto *work = encode_work(&finish_handle_.get(), WorkType::Common);
        io_uring_sqe_set_data(sqe, work);

        finish_handle_.get().maybe_set_cancel(context.runtime());
    }

private:
    Func prep_func_;
    HandleBox<Handle> finish_handle_;
};

template <unsigned int Flags, typename Sender, typename Receiver>
class FlaggedOpState {
public:
    FlaggedOpState(Sender sender, Receiver receiver)
        : op_state_(sender.connect(std::move(receiver))) {}

    FlaggedOpState(FlaggedOpState &&) = delete;
    FlaggedOpState &operator=(FlaggedOpState &&) = delete;
    FlaggedOpState(const FlaggedOpState &) = delete;
    FlaggedOpState &operator=(const FlaggedOpState &) = delete;

    void start(unsigned int flags) noexcept { op_state_.start(flags | Flags); }

private:
    using OperationState =
        decltype(std::declval<Sender>().connect(std::declval<Receiver>()));
    OperationState op_state_;
};

template <typename Receiver> class WhenAnyCanceller {
public:
    using TokenType = decltype(std::declval<Receiver>().get_stop_token());

    auto chain_token(TokenType token) noexcept {
        if (token.stop_possible()) {
            stop_callback_.emplace(std::move(token), Cancellation{this});
        }
        return stop_source_.get_token();
    }

    void maybe_request_stop() noexcept { stop_source_.request_stop(); }

    void maybe_reset() noexcept { stop_callback_.reset(); }

private:
    struct Cancellation {
        WhenAnyCanceller *self;
        void operator()() noexcept { self->cancel_(); }
    };
    void cancel_() noexcept { stop_source_.request_stop(); }

    using StopCallbackType =
        typename stop_callback_of<TokenType>::template type<Cancellation>;

    std::stop_source stop_source_;
    std::optional<StopCallbackType> stop_callback_;
};

template <typename Receiver> class WhenAllCanceller {
public:
    using TokenType = decltype(std::declval<Receiver>().get_stop_token());

    auto chain_token(TokenType token) noexcept { return token; }

    void maybe_request_stop() noexcept {}

    void maybe_reset() noexcept {}
};

template <typename Receiver, typename Canceller, typename... Senders>
class ParallelOperationState {
public:
    ParallelOperationState(std::tuple<Senders...> senders, Receiver receiver)
        : receiver_(std::move(receiver)) {
        auto next_token = canceller_.chain_token(receiver_.get_stop_token());
        connect_senders_(senders, next_token);
    }

    ParallelOperationState(ParallelOperationState &&) = delete;
    ParallelOperationState &operator=(ParallelOperationState &&) = delete;
    ParallelOperationState(const ParallelOperationState &) = delete;
    ParallelOperationState &operator=(const ParallelOperationState &) = delete;

    ~ParallelOperationState() {
        std::apply([](auto &&...states) { (states.destroy(), ...); },
                   op_states_);
    }

    void start(unsigned int flags) noexcept {
        if constexpr (sizeof...(Senders) == 0) {
            std::move(receiver_)(
                std::make_pair(std::move(order_), std::move(results_)));
        } else {
            std::apply(
                [&](auto &&...states) { (states.get().start(flags), ...); },
                op_states_);
        }
    }

private:
    using ReceiverTokenType =
        decltype(std::declval<Receiver>().get_stop_token());
    using TokenType = decltype(std::declval<Canceller>().chain_token(
        std::declval<ReceiverTokenType>()));

    template <size_t I = 0>
    void connect_senders_(std::tuple<Senders...> &senders,
                          const TokenType &token) noexcept {
        if constexpr (I < sizeof...(Senders)) {
            std::get<I>(op_states_).accept([&] {
                return std::move(std::get<I>(senders))
                    .connect(ChildReceiver<I>{this, token});
            });
            connect_senders_<I + 1>(senders, token);
        }
    }

    template <size_t I, typename R> void receive_(R &&result) noexcept {
        canceller_.maybe_request_stop();
        auto no = completed_count_++;
        order_[no] = I;
        std::get<I>(results_) = std::forward<R>(result);
        if (no + 1 == sizeof...(Senders)) {
            canceller_.maybe_reset();
            std::move(receiver_)(
                std::make_pair(std::move(order_), std::move(results_)));
        }
    }

    template <size_t I> struct ChildReceiver {
        ParallelOperationState *self;
        TokenType stop_token;
        template <typename R> void operator()(R &&result) noexcept {
            self->receive_<I>(std::forward<R>(result));
        }
        auto get_stop_token() const noexcept { return stop_token; }
    };

    template <typename T> struct operation_state_traits;
    template <size_t... Is>
    struct operation_state_traits<std::index_sequence<Is...>> {
        using type =
            std::tuple<RawStorage<decltype(std::declval<Senders>().connect(
                std::declval<ChildReceiver<Is>>()))>...>;
    };
    using OperationStates = typename operation_state_traits<
        std::make_index_sequence<sizeof...(Senders)>>::type;

protected:
    OperationStates op_states_;
    std::array<size_t, sizeof...(Senders)> order_;
    std::tuple<typename Senders::ReturnType...> results_;
    size_t completed_count_ = 0;
    Receiver receiver_;
    Canceller canceller_;
};

template <typename Receiver, typename... Senders>
using ParallelAnyOperationState =
    ParallelOperationState<Receiver, WhenAnyCanceller<Receiver>, Senders...>;

template <typename Receiver, typename... Senders>
using ParallelAllOperationState =
    ParallelOperationState<Receiver, WhenAllCanceller<Receiver>, Senders...>;

template <typename Receiver> struct ReceiverAllWrapper {
    Receiver receiver;
    ReceiverAllWrapper(Receiver receiver) : receiver(std::move(receiver)) {}
    template <typename R> void operator()(R &&result) noexcept {
        auto &[order, results] = result;
        std::move(receiver)(std::move(results));
    }
    auto get_stop_token() const noexcept { return receiver.get_stop_token(); }
};

template <typename Receiver> struct ReceiverAnyWrapper {
    Receiver receiver;
    ReceiverAnyWrapper(Receiver receiver) : receiver(std::move(receiver)) {}
    template <typename R> void operator()(R &&result) noexcept {
        auto &[order, results] = result;
        size_t index = order[0];
        std::move(receiver)(tuple_at(results, index));
    }
    auto get_stop_token() const noexcept { return receiver.get_stop_token(); }
};

template <typename Receiver, typename... Senders>
using WhenAnyOperationState =
    ParallelAnyOperationState<ReceiverAnyWrapper<Receiver>, Senders...>;

template <typename Receiver, typename... Senders>
using WhenAllOperationState =
    ParallelAllOperationState<ReceiverAllWrapper<Receiver>, Senders...>;

template <typename Receiver, unsigned int Flags, typename... Senders>
class LinkOperationState : public WhenAllOperationState<Receiver, Senders...> {
public:
    using Base = WhenAllOperationState<Receiver, Senders...>;
    using Base::Base;

    void start(unsigned int flags) noexcept {
        auto *ring = detail::Context::current().ring();
        ring->reserve_space(sizeof...(Senders));
        start_linked_operations_(flags);
    }

private:
    template <size_t I = 0>
    void start_linked_operations_(unsigned int flags) noexcept {
        if constexpr (I < sizeof...(Senders)) {
            auto &state = std::get<I>(Base::op_states_);
            if constexpr (I < sizeof...(Senders) - 1) {
                state.get().start(flags | Flags);
            } else {
                state.get().start(flags);
            }
            start_linked_operations_<I + 1>(flags);
        }
    }
};

template <typename Receiver, typename Canceller, typename Sender>
class RangedParallelOperationState {
public:
    RangedParallelOperationState(std::vector<Sender> senders, Receiver receiver)
        : op_states_(senders.size()), order_(senders.size()),
          results_(senders.size()), receiver_(std::move(receiver)) {
        auto next_token = canceller_.chain_token(receiver_.get_stop_token());
        for (size_t i = 0; i < senders.size(); ++i) {
            op_states_[i].accept([&] {
                return std::move(senders[i])
                    .connect(ChildReceiver{this, i, next_token});
            });
        }
    }

    RangedParallelOperationState(RangedParallelOperationState &&) = delete;
    RangedParallelOperationState &
    operator=(RangedParallelOperationState &&) = delete;
    RangedParallelOperationState(const RangedParallelOperationState &) = delete;
    RangedParallelOperationState &
    operator=(const RangedParallelOperationState &) = delete;

    ~RangedParallelOperationState() {
        for (auto &op_state : op_states_) {
            op_state.destroy();
        }
    }

    void start(unsigned int flags) noexcept {
        if (op_states_.empty()) {
            std::move(receiver_)(
                std::make_pair(std::move(order_), std::move(results_)));
        } else {
            for (auto &op_state : op_states_) {
                op_state.get().start(flags);
            }
        }
    }

private:
    using ReceiverTokenType =
        decltype(std::declval<Receiver>().get_stop_token());
    using TokenType = decltype(std::declval<Canceller>().chain_token(
        std::declval<ReceiverTokenType>()));

    void receive_(size_t index, auto &&result) noexcept {
        canceller_.maybe_request_stop();
        size_t no = completed_count_++;
        order_[no] = index;
        results_[index] = std::forward<decltype(result)>(result);
        if (no + 1 == op_states_.size()) {
            std::move(receiver_)(
                std::make_pair(std::move(order_), std::move(results_)));
        }
    }

    struct ChildReceiver {
        RangedParallelOperationState *self;
        size_t index;
        TokenType stop_token;
        template <typename R> void operator()(R &&result) noexcept {
            self->receive_(index, std::forward<R>(result));
        }
        auto get_stop_token() const noexcept { return stop_token; }
    };

    using OperationStates =
        std::vector<RawStorage<decltype(std::declval<Sender>().connect(
            std::declval<ChildReceiver>()))>>;

protected:
    OperationStates op_states_;
    std::vector<size_t> order_;
    std::vector<typename Sender::ReturnType> results_;
    size_t completed_count_ = 0;
    Receiver receiver_;
    Canceller canceller_;
};

template <typename Receiver, typename Sender>
using RangedParallelAllOperationState =
    RangedParallelOperationState<Receiver, WhenAllCanceller<Receiver>, Sender>;

template <typename Receiver, typename Sender>
using RangedParallelAnyOperationState =
    RangedParallelOperationState<Receiver, WhenAnyCanceller<Receiver>, Sender>;

template <typename Receiver>
using ReceiverRangedAllWrapper = ReceiverAllWrapper<Receiver>;

template <typename Receiver> struct ReceiverRangedAnyWrapper {
    Receiver receiver;
    ReceiverRangedAnyWrapper(Receiver receiver)
        : receiver(std::move(receiver)) {}
    template <typename R> void operator()(R &&result) noexcept {
        auto &[order, results] = result;
        size_t index = order[0];
        std::move(receiver)(std::make_pair(index, std::move(results[index])));
    }
    auto get_stop_token() const noexcept { return receiver.get_stop_token(); }
};

template <typename Receiver, typename Sender>
using WhenAllRangeOperationState =
    RangedParallelAllOperationState<ReceiverRangedAllWrapper<Receiver>, Sender>;

template <typename Receiver, typename Sender>
using WhenAnyRangeOperationState =
    RangedParallelAnyOperationState<ReceiverRangedAnyWrapper<Receiver>, Sender>;

template <typename Receiver, unsigned int Flags, typename Sender>
class RangedLinkOperationState
    : public WhenAllRangeOperationState<Receiver, Sender> {
public:
    using Base = WhenAllRangeOperationState<Receiver, Sender>;
    using Base::Base;

    void start(unsigned int flags) noexcept {
        auto *ring = detail::Context::current().ring();
        ring->reserve_space(Base::op_states_.size());
        for (size_t i = 0; i < Base::op_states_.size(); ++i) {
            auto &op_state = Base::op_states_[i];
            if (i < Base::op_states_.size() - 1) {
                op_state.get().start(flags | Flags);
            } else {
                op_state.get().start(flags);
            }
        }
    }
};

} // namespace detail

} // namespace condy