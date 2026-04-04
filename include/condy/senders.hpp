#pragma once

#include "condy/awaiters.hpp"
#include "condy/cqe_handler.hpp"
#include "condy/invoker.hpp"
#include "condy/utils.hpp"
#include <atomic>

namespace condy {

template <OpFinishHandleLike Handle, PrepFuncLike Func, typename... HandleArgs>
class OpSenderBase {
public:
    using ReturnType = typename Handle::ReturnType;

    OpSenderBase(Func func, HandleArgs &&...args)
        : prep_func_(std::move(func)),
          handle_args_(std::forward<HandleArgs>(args)...) {}

    template <typename Receiver>
    class OperationState : public InvokerAdapter<OperationState<Receiver>> {
    public:
        template <typename... Args>
        OperationState(Func prep_func, HandleBox<Handle> finish_handle,
                       Receiver receiver)
            : prep_func_(std::move(prep_func)),
              finish_handle_(std::move(finish_handle)),
              receiver_(std::move(receiver)) {}

        OperationState(OperationState &&) = delete;
        OperationState &operator=(OperationState &&) = delete;
        OperationState(const OperationState &) = delete;
        OperationState &operator=(const OperationState &) = delete;

    public:
        void start() noexcept {
            auto &context = detail::Context::current();
            auto *ring = context.ring();
            context.runtime()->pend_work();
            finish_handle_.get().set_invoker(this);
            io_uring_sqe *sqe = prep_func_(ring);
            assert(sqe && "prep_func must return a valid sqe");
            // TODO: support flags
            // io_uring_sqe_set_flags(sqe, sqe->flags | flags);
            auto *work = encode_work(&finish_handle_.get(), WorkType::Common);
            io_uring_sqe_set_data(sqe, work);
        }

        void cancel(Runtime *runtime) noexcept {
            finish_handle_.get().cancel(runtime);
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

    template <typename Receiver> auto connect(Receiver receiver) noexcept {
        return OperationState<Receiver>(
            prep_func_,
            std::apply(
                [](auto &&...args) {
                    return HandleBox(
                        Handle(std::forward<decltype(args)>(args)...));
                },
                std::move(handle_args_)),
            std::move(receiver));
    }

private:
    Func prep_func_;
    std::tuple<HandleArgs &&...> handle_args_;
};

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename... HandleArgs>
using OpSender =
    OpSenderBase<OpFinishHandle<CQEHandler>, PrepFunc, HandleArgs...>;

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler,
          typename MultiShotFunc, typename... HandleArgs>
using MultiShotOpSender =
    OpSenderBase<MultiShotOpFinishHandle<CQEHandler, MultiShotFunc>, PrepFunc,
                 HandleArgs...>;

template <PrepFuncLike PrepFunc, CQEHandlerLike CQEHandler, typename FreeFunc,
          typename... HandleArgs>
using ZeroCopyOpSender =
    OpSenderBase<ZeroCopyOpFinishHandle<CQEHandler, FreeFunc>, PrepFunc,
                 HandleArgs...>;

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename... Args>
auto build_op_sender(PrepFunc &&prep_func, Args &&...args) {
    return OpSender<std::decay_t<PrepFunc>, CQEHandler, Args...>(
        std::forward<PrepFunc>(prep_func), std::forward<Args>(args)...);
}

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc,
          typename MultiShotFunc, typename... Args>
auto build_multishot_op_sender(PrepFunc &&func, MultiShotFunc &&multishot_func,
                               Args &&...handler_args) {
    return MultiShotOpSender<std::decay_t<PrepFunc>, CQEHandler,
                             std::decay_t<MultiShotFunc>, Args...>(
        std::forward<PrepFunc>(func),
        std::forward<MultiShotFunc>(multishot_func),
        std::forward<Args>(handler_args)...);
}

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename FreeFunc,
          typename... Args>
auto build_zero_copy_op_sender(PrepFunc &&func, FreeFunc &&free_func,
                               Args &&...handler_args) {
    return ZeroCopyOpSender<std::decay_t<PrepFunc>, CQEHandler,
                            std::decay_t<FreeFunc>, Args...>(
        std::forward<PrepFunc>(func), std::forward<FreeFunc>(free_func),
        std::forward<Args>(handler_args)...);
}

template <bool Cancel, typename... Senders> class ParallelSender {
public:
    using ReturnType = std::pair<std::array<size_t, sizeof...(Senders)>,
                                 std::tuple<typename Senders::ReturnType...>>;

    ParallelSender(Senders... senders) : senders_(std::move(senders)...) {}

    template <typename Receiver> auto connect(Receiver receiver) {
        return OperationState<Receiver>(std::move(senders_),
                                        std::move(receiver));
    }

    template <typename Receiver> class OperationState {
    public:
        OperationState(std::tuple<Senders...> senders, Receiver receiver)
            : receiver_(std::move(receiver)) {
            connect_senders_(std::move(senders),
                             std::make_index_sequence<sizeof...(Senders)>{});
        }

        ~OperationState() {
            std::apply([](auto &&...state) { (state.destroy(), ...); },
                       operation_states_);
        }

        void start() noexcept {
            std::apply([&](auto &&...state) { (state.get().start(), ...); },
                       operation_states_);
        }

        void cancel(Runtime *runtime) noexcept {
            if constexpr (Cancel) {
                if (canceled_.exchange(true)) {
                    return;
                }
                std::apply(
                    [&](auto &&...state) {
                        (state.get().cancel(runtime), ...);
                    },
                    operation_states_);
            }
        }

    private:
        template <size_t... Is>
        void connect_senders_(std::tuple<Senders...> senders,
                              std::index_sequence<Is...>) {
            (std::get<Is>(operation_states_).accept([&] {
                return std::get<Is>(senders).connect(ReceiverImpl<Is>{this});
            }),
             ...);
        }

        template <size_t Idx> void check_and_invoke_() {
            auto no = completed_count_++;
            order_[no] = Idx;

            if constexpr (Cancel) {
                if (!canceled_.exchange(true)) {
                    auto *runtime = detail::Context::current().runtime();
                    foreach_call_cancel_<Idx>(runtime);
                }
            }

            if (no == sizeof...(Senders) - 1) {
                std::move(receiver_)(std::make_pair(order_, results_));
            }
        }

        template <size_t SkipIdx, size_t I = 0>
        void foreach_call_cancel_(Runtime *runtime) noexcept {
            if constexpr (I < sizeof...(Senders)) {
                auto &state = std::get<I>(operation_states_);
                if constexpr (I != SkipIdx) {
                    state.get().cancel(runtime);
                }
                foreach_call_cancel_<SkipIdx, I + 1>(runtime);
            }
        }

        template <size_t I> struct ReceiverImpl {
            OperationState *state;

            void operator()(typename std::tuple_element_t<
                            I, std::tuple<Senders...>>::ReturnType result) {
                std::get<I>(state->results_) = std::move(result);
                state->check_and_invoke_<I>();
            }
        };

        template <typename T> struct operation_state_traits;

        template <size_t... Is>
        struct operation_state_traits<std::index_sequence<Is...>> {
            using type =
                std::tuple<RawStorage<decltype(std::declval<Senders>().connect(
                    std::declval<ReceiverImpl<Is>>()))>...>;
        };

        using OperationStates = typename operation_state_traits<
            std::make_index_sequence<sizeof...(Senders)>>::type;
        OperationStates operation_states_;
        size_t completed_count_ = 0;
        std::array<size_t, sizeof...(Senders)> order_{};
        std::tuple<typename Senders::ReturnType...> results_;
        Receiver receiver_;
        std::atomic_bool canceled_ = false;
    };

    std::tuple<Senders...> senders_;
};

template <typename... Senders>
class WhenAllSender : public ParallelSender<false, Senders...> {
public:
    using Base = ParallelSender<false, Senders...>;
    using ReturnType = std::tuple<typename Senders::ReturnType...>;

    WhenAllSender(Senders... senders) : Base(std::move(senders)...) {}

    template <typename Receiver> auto connect(Receiver receiver) {
        return Base::connect(ReceiverAdapter<Receiver>{std::move(receiver)});
    }

private:
    template <typename Receiver> struct ReceiverAdapter {
        Receiver receiver;

        template <typename... Args>
        void operator()(std::pair<std::array<size_t, sizeof...(Senders)>,
                                  std::tuple<Args...>>
                            result) {
            auto &[order, results] = result;
            std::move(receiver)(std::move(results));
        }
    };
};

template <typename... Senders>
class WhenAnySender : public ParallelSender<true, Senders...> {
public:
    using Base = ParallelSender<true, Senders...>;
    using ReturnType = std::variant<typename Senders::ReturnType...>;

    WhenAnySender(Senders... senders) : Base(std::move(senders)...) {}

    template <typename Receiver> auto connect(Receiver receiver) {
        return Base::connect(ReceiverAdapter<Receiver>{std::move(receiver)});
    }

private:
    template <typename Receiver> struct ReceiverAdapter {
        Receiver receiver;

        template <typename... Args>
        void operator()(std::pair<std::array<size_t, sizeof...(Senders)>,
                                  std::tuple<Args...>>
                            result) {
            auto &[order, results] = result;
            auto idx = order[0];
            std::move(receiver)(tuple_at_(results, idx));
        }
    };

    template <size_t Idx = 0>
    static auto tuple_at_(std::tuple<typename Senders::ReturnType...> &results,
                          size_t idx) {
        if constexpr (Idx < sizeof...(Senders)) {
            if (idx == Idx) {
                return ReturnType{std::in_place_index<Idx>,
                                  std::move(std::get<Idx>(results))};
            } else {
                return tuple_at_<Idx + 1>(results, idx);
            }
        } else {
            // Should not reach here, but we need to make compiler happy.
            // Throwing an exception will lead to wrong optimization.
            assert(false && "Index out of bounds");
            return ReturnType{std::in_place_index<0>,
                              std::move(std::get<0>(results))};
        }
    }
};

template <typename... Senders> auto when_all_senders(Senders &&...senders) {
    return WhenAllSender<std::decay_t<Senders>...>(
        std::forward<Senders>(senders)...);
}

template <typename... Senders> auto when_any_senders(Senders &&...senders) {
    return WhenAnySender<std::decay_t<Senders>...>(
        std::forward<Senders>(senders)...);
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

template <typename Sender> class SenderAwaiter {
public:
    SenderAwaiter(Sender sender) : sender_(std::move(sender)) {}

    ~SenderAwaiter() { operation_state_.destroy(); }

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) noexcept {
        operation_state_.accept(
            [&] { return sender_.connect(Receiver{h, this}); });
        operation_state_.get().start();
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

} // namespace detail

template <typename Sender> auto as_awaiter(Sender &&sender) {
    return detail::SenderAwaiter<std::decay_t<Sender>>(
        std::forward<Sender>(sender));
}

} // namespace condy