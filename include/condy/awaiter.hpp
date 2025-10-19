#pragma once

#include "condy/finish_handle.hpp"
#include <coroutine>

namespace condy {

template <typename Handle, typename Awaiter> class RangedParallelAwaiter {
public:
    using HandleType = Handle;

    template <typename Range,
              typename = std::enable_if_t<!std::same_as<
                  std::remove_cvref_t<Range>, RangedParallelAwaiter>>>
    RangedParallelAwaiter(Range &&awaiters)
        : awaiters_(std::begin(awaiters), std::end(awaiters)) {}

public:
    HandleType *get_handle() { return &finish_handle_; }

    void init_finish_handle() {
        using ChildHandle = typename Awaiter::HandleType;
        std::vector<ChildHandle *> handles;
        handles.reserve(awaiters_.size());
        for (auto &awaiter : awaiters_) {
            awaiter.init_finish_handle();
            handles.push_back(awaiter.get_handle());
        }
        finish_handle_.init(std::move(handles));
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        init_finish_handle();
        finish_handle_.set_on_finish(
            [h, this](typename HandleType::ReturnType r) {
                result_ = std::move(r);
                h.resume();
            });
    }

    typename Handle::ReturnType await_resume() { return std::move(result_); }

private:
    HandleType finish_handle_;
    typename HandleType::ReturnType result_;
    std::vector<Awaiter> awaiters_;
};

template <typename Awaiter>
using RangedWaitAllAwaiter = RangedParallelAwaiter<
    RangedWaitAllFinishHandle<typename Awaiter::HandleType>, Awaiter>;

template <typename Awaiter>
using RangedWaitOneAwaiter = RangedParallelAwaiter<
    RangedWaitOneFinishHandle<typename Awaiter::HandleType>, Awaiter>;

} // namespace condy