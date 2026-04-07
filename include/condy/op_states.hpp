#pragma once

#include "condy/concepts.hpp"
#include "condy/condy_uring.hpp"
#include "condy/finish_handles.hpp"
#include "condy/invoker.hpp"
#include "condy/utils.hpp"
#include <stop_token>

namespace condy {
namespace detail {

template <OpFinishHandleLike Handle, PrepFuncLike Func, typename Receiver>
class OpSenderOperationState
    : public InvokerAdapter<OpSenderOperationState<Handle, Func, Receiver>> {
public:
    OpSenderOperationState(Func prep_func, HandleBox<Handle> finish_handle,
                           Receiver receiver)
        : prep_func_(std::move(prep_func)),
          finish_handle_(std::move(finish_handle)),
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
        finish_handle_.maybe_release();
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
        canceller_.set_token(receiver_.get_stop_token());
        std::apply([&](auto &&...states) { (states.get().start(flags), ...); },
                   op_states_);
    }

private:
    template <size_t I = 0>
    void connect_senders_(std::tuple<Senders...> &senders) noexcept {
        if constexpr (I < sizeof...(Senders)) {
            std::get<I>(op_states_).accept([&] {
                return std::move(std::get<I>(senders))
                    .connect(ChildReceiver<I>{this, canceller_.get_token()});
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
        std::stop_token stop_token;
        template <typename R> void operator()(R &&result) noexcept {
            self->receive_<I>(std::forward<R>(result));
        }

        std::stop_token get_stop_token() const noexcept { return stop_token; }
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

template <typename Receiver> struct ReceiverWrapper {
    Receiver receiver;
    template <typename R> void operator()(R &&result) noexcept {
        auto &[order, results] = result;
        std::move(receiver)(std::move(results));
    }
    std::stop_token get_stop_token() const noexcept {
        return receiver.get_stop_token();
    }
};

template <typename Receiver, typename... Senders>
class WhenAnyOperationState
    : public ParallelAnyOperationState<ReceiverWrapper<Receiver>, Senders...> {
public:
    using Base =
        ParallelAnyOperationState<ReceiverWrapper<Receiver>, Senders...>;

    WhenAnyOperationState(std::tuple<Senders...> senders, Receiver receiver)
        : Base(std::move(senders),
               ReceiverWrapper<Receiver>{std::move(receiver)}) {}

    void start(unsigned int flags) noexcept { Base::start(flags); }
};

template <typename Receiver, typename... Senders>
class WhenAllOperationState
    : public ParallelAllOperationState<ReceiverWrapper<Receiver>, Senders...> {
public:
    using Base =
        ParallelAllOperationState<ReceiverWrapper<Receiver>, Senders...>;

    WhenAllOperationState(std::tuple<Senders...> senders, Receiver receiver)
        : Base(std::move(senders),
               ReceiverWrapper<Receiver>{std::move(receiver)}) {}

    void start(unsigned int flags) noexcept { Base::start(flags); }
};

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

} // namespace detail

} // namespace condy