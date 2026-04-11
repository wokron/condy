#pragma once

#include "condy/concepts.hpp"
#include "condy/condy_uring.hpp"
#include "condy/finish_handles.hpp"
#include "condy/invoker.hpp"
#include "condy/utils.hpp"
#include <cstddef>
#include <stop_token>

namespace condy {
namespace detail {

template <OpFinishHandleLike Handle, PrepFuncLike Func, typename Receiver>
class OpSenderOperationState
    : public InvokerAdapter<OpSenderOperationState<Handle, Func, Receiver>> {
public:
    template <typename... HandleArgs>
    OpSenderOperationState(Func prep_func, Receiver receiver,
                           HandleArgs &&...handle_args)
        : prep_func_(std::move(prep_func)),
          finish_handle_(std::forward<HandleArgs>(handle_args)...),
          receiver_(std::move(receiver)) {}

    OpSenderOperationState(OpSenderOperationState &&) = delete;
    OpSenderOperationState &operator=(OpSenderOperationState &&) = delete;
    OpSenderOperationState(const OpSenderOperationState &) = delete;
    OpSenderOperationState &operator=(const OpSenderOperationState &) = delete;

public:
    void start(unsigned int flags) noexcept {
        auto &context = detail::Context::current();
        auto *ring = context.ring();
        context.runtime()->pend_work();
        finish_handle_.get().set_invoker(this);
        io_uring_sqe *sqe = prep_func_(ring);
        assert(sqe && "prep_func must return a valid sqe");
        io_uring_sqe_set_flags(sqe, sqe->flags | flags);
        auto *work = encode_work(&finish_handle_.get(), WorkType::Common);
        io_uring_sqe_set_data(sqe, work);

        auto stop_token = receiver_.get_stop_token();
        if (stop_token.stop_possible()) {
            stop_callback_.emplace(std::move(stop_token),
                                   Cancellation{this, context.runtime()});
        }
    }

    void invoke() noexcept {
        stop_callback_.reset();
        auto result = finish_handle_.get().extract_result();
        std::move(receiver_)(std::move(result));
    }

private:
    struct Cancellation {
        OpSenderOperationState *self;
        Runtime *runtime;
        void operator()() noexcept { self->cancel_(runtime); }
    };

    void cancel_(Runtime *runtime) noexcept {
        finish_handle_.get().cancel(runtime);
    }

    Func prep_func_;
    HandleBox<Handle> finish_handle_;
    Receiver receiver_;
    std::optional<std::stop_callback<Cancellation>> stop_callback_;
};

template <unsigned int Flags, typename Sender, typename Receiver>
class FlaggedOpState {
public:
    FlaggedOpState(Sender sender, Receiver receiver) {
        op_state_.accept([&] { return sender.connect(std::move(receiver)); });
    }
    ~FlaggedOpState() { op_state_.destroy(); }

    FlaggedOpState(FlaggedOpState &&) = delete;
    FlaggedOpState &operator=(FlaggedOpState &&) = delete;
    FlaggedOpState(const FlaggedOpState &) = delete;
    FlaggedOpState &operator=(const FlaggedOpState &) = delete;

    void start(unsigned int flags) noexcept {
        op_state_.get().start(flags | Flags);
    }

private:
    using OperationState =
        decltype(std::declval<Sender>().connect(std::declval<Receiver>()));
    RawStorage<OperationState> op_state_;
};

class WhenAnyCanceller {
public:
    void set_token(std::stop_token token) noexcept {
        if (token.stop_possible()) {
            stop_callback_.emplace(std::move(token), Cancellation{this});
        }
    }

    void maybe_request_stop() noexcept { stop_source_.request_stop(); }

    std::stop_token get_token() noexcept { return stop_source_.get_token(); }

private:
    struct Cancellation {
        WhenAnyCanceller *self;
        void operator()() noexcept { self->cancel_(); }
    };
    void cancel_() noexcept { stop_source_.request_stop(); }

    std::stop_source stop_source_;
    std::optional<std::stop_callback<Cancellation>> stop_callback_;
};

class WhenAllCanceller {
public:
    void set_token(std::stop_token token) noexcept {
        stop_token_ = std::move(token);
    }

    std::stop_token get_token() const noexcept { return stop_token_; }

    void maybe_request_stop() noexcept {}

private:
    std::stop_token stop_token_;
};

template <typename Receiver, typename Canceller, typename... Senders>
class ParallelOperationState {
public:
    ParallelOperationState(std::tuple<Senders...> senders, Receiver receiver)
        : receiver_(std::move(receiver)) {
        connect_senders_(senders);
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
            std::move(receiver_)(std::make_pair(order_, results_));
        } else {
            canceller_.set_token(receiver_.get_stop_token());
            std::apply(
                [&](auto &&...states) { (states.get().start(flags), ...); },
                op_states_);
        }
    }

private:
    template <size_t I = 0>
    void connect_senders_(std::tuple<Senders...> &senders) noexcept {
        if constexpr (I < sizeof...(Senders)) {
            std::get<I>(op_states_).accept([&] {
                return std::move(std::get<I>(senders))
                    .connect(ChildReceiver<I>{this});
            });
            connect_senders_<I + 1>(senders);
        }
    }

    template <size_t I, typename R> void receive_(R &&result) noexcept {
        canceller_.maybe_request_stop();
        auto no = completed_count_++;
        order_[no] = I;
        std::get<I>(results_) = std::forward<R>(result);
        if (no + 1 == sizeof...(Senders)) {
            std::move(receiver_)(std::make_pair(order_, results_));
        }
    }

    template <size_t I> struct ChildReceiver {
        ParallelOperationState *self;
        template <typename R> void operator()(R &&result) noexcept {
            self->receive_<I>(std::forward<R>(result));
        }

        std::stop_token get_stop_token() const noexcept {
            return self->canceller_.get_token();
        }
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
    ParallelOperationState<Receiver, WhenAnyCanceller, Senders...>;

template <typename Receiver, typename... Senders>
using ParallelAllOperationState =
    ParallelOperationState<Receiver, WhenAllCanceller, Senders...>;

template <typename Receiver> struct ReceiverAllWrapper {
    Receiver receiver;
    ReceiverAllWrapper(Receiver receiver) : receiver(std::move(receiver)) {}
    template <typename R> void operator()(R &&result) noexcept {
        auto &[order, results] = result;
        std::move(receiver)(std::move(results));
    }
    std::stop_token get_stop_token() const noexcept {
        return receiver.get_stop_token();
    }
};

template <typename Receiver> struct ReceiverAnyWrapper {
    Receiver receiver;
    ReceiverAnyWrapper(Receiver receiver) : receiver(std::move(receiver)) {}
    template <typename R> void operator()(R &&result) noexcept {
        auto &[order, results] = result;
        size_t index = order[0];
        std::move(receiver)(tuple_at(results, index));
    }
    std::stop_token get_stop_token() const noexcept {
        return receiver.get_stop_token();
    }
};

template <typename Receiver, typename... Senders>
    requires(sizeof...(Senders) > 0)
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
        for (size_t i = 0; i < senders.size(); ++i) {
            op_states_[i].accept([&] {
                return std::move(senders[i]).connect(ChildReceiver{this, i});
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
            std::move(receiver_)(std::make_pair(order_, results_));
        } else {
            canceller_.set_token(receiver_.get_stop_token());
            for (auto &op_state : op_states_) {
                op_state.get().start(flags);
            }
        }
    }

private:
    void receive_(size_t index, auto &&result) noexcept {
        canceller_.maybe_request_stop();
        size_t no = completed_count_++;
        order_[no] = index;
        results_[index] = std::forward<decltype(result)>(result);
        if (no + 1 == op_states_.size()) {
            std::move(receiver_)(std::make_pair(order_, results_));
        }
    }

    struct ChildReceiver {
        RangedParallelOperationState *self;
        size_t index;
        template <typename R> void operator()(R &&result) noexcept {
            self->receive_(index, std::forward<R>(result));
        }

        std::stop_token get_stop_token() const noexcept {
            return self->canceller_.get_token();
        }
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
    RangedParallelOperationState<Receiver, WhenAllCanceller, Sender>;

template <typename Receiver, typename Sender>
using RangedParallelAnyOperationState =
    RangedParallelOperationState<Receiver, WhenAnyCanceller, Sender>;

template <typename Receiver, typename Sender>
using WhenAllRangeOperationState =
    RangedParallelAllOperationState<ReceiverAllWrapper<Receiver>, Sender>;

template <typename Receiver> struct ReceiverRangedAnyWrapper {
    Receiver receiver;
    ReceiverRangedAnyWrapper(Receiver receiver)
        : receiver(std::move(receiver)) {}
    template <typename R> void operator()(R &&result) noexcept {
        auto &[order, results] = result;
        size_t index = order[0];
        std::move(receiver)(std::make_pair(index, std::move(results[index])));
    }
    std::stop_token get_stop_token() const noexcept {
        return receiver.get_stop_token();
    }
};

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