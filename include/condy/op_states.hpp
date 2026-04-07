#pragma once

#include "condy/concepts.hpp"
#include "condy/condy_uring.hpp"
#include "condy/finish_handles.hpp"
#include "condy/invoker.hpp"
#include "condy/utils.hpp"

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
    }

    void invoke() noexcept {
        auto result = finish_handle_.get().extract_result();
        finish_handle_.maybe_release();
        std::move(receiver_)(std::move(result));
    }

private:
    Func prep_func_;
    HandleBox<Handle> finish_handle_;
    Receiver receiver_;
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

    void start(unsigned int flags) noexcept { op_state_.get().start(flags | Flags); }

private:
    using OperationState =
        decltype(std::declval<Sender>().connect(std::declval<Receiver>()));
    RawStorage<OperationState> op_state_;
};

template <typename Receiver, typename... Senders> class WhenAllOperationState {
public:
    WhenAllOperationState(std::tuple<Senders...> senders, Receiver receiver)
        : receiver_(std::move(receiver)) {
        connect_senders_(senders);
    }

    WhenAllOperationState(WhenAllOperationState &&) = delete;
    WhenAllOperationState &operator=(WhenAllOperationState &&) = delete;
    WhenAllOperationState(const WhenAllOperationState &) = delete;
    WhenAllOperationState &operator=(const WhenAllOperationState &) = delete;

    ~WhenAllOperationState() {
        std::apply([](auto &&...states) { (states.destroy(), ...); },
                   op_states_);
    }

    void start(unsigned int flags) noexcept {
        std::apply([&](auto &&...states) { (states.get().start(flags), ...); },
                   op_states_);
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
        auto no = completed_count_++;
        std::get<I>(results_) = std::forward<R>(result);
        if (no + 1 == sizeof...(Senders)) {
            std::move(receiver_)(std::move(results_));
        }
    }

    template <size_t I> struct ChildReceiver {
        WhenAllOperationState *self;
        template <typename R> void operator()(R &&result) noexcept {
            self->receive_<I>(std::forward<R>(result));
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
    std::tuple<typename Senders::ReturnType...> results_;
    size_t completed_count_ = 0;
    Receiver receiver_;
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