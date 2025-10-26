#pragma once

#include "condy/finish_handles.hpp"
#include <coroutine>
#include <utility>

namespace condy {

template <typename Func> class [[nodiscard]] RetryAwaiter {
public:
    RetryAwaiter(Func func) : func_(std::move(func)) {}
    RetryAwaiter(RetryAwaiter &&) = default;

    RetryAwaiter(const RetryAwaiter &) = delete;
    RetryAwaiter &operator=(const RetryAwaiter &) = delete;
    RetryAwaiter &operator=(RetryAwaiter &&) = delete;

public:
    bool await_ready() { return func_(); }

    template <typename PromiseType>
    void await_suspend(std::coroutine_handle<PromiseType> h) {
        handle_.set_on_retry(
            [h, this] {
                bool ok = func_();
                return ok;
            },
            h);
        handle_.prep_retry();
    }

    void await_resume() {}

private:
    RetryFinishHandle<> handle_;
    Func func_;
};

template <typename Func> auto retry(Func &&func) {
    return RetryAwaiter<std::decay_t<Func>>(std::forward<Func>(func));
}

} // namespace condy