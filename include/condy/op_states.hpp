#pragma once

#include "condy/concepts.hpp"
#include "condy/condy_uring.hpp"
#include "condy/finish_handles.hpp"
#include "condy/invoker.hpp"

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

} // namespace detail

} // namespace condy