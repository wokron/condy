#pragma once

#include "condy/awaiters.hpp"
#include "condy/cqe_handler.hpp"
#include "condy/invoker.hpp"
#include "condy/utils.hpp"

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

template <CQEHandlerLike CQEHandler, PrepFuncLike PrepFunc, typename... Args>
auto build_op_sender(PrepFunc &&prep_func, Args &&...args) {
    return OpSender<PrepFunc, CQEHandler, Args...>(
        std::forward<PrepFunc>(prep_func), std::forward<Args>(args)...);
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