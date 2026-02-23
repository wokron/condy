#pragma once

#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include <atomic>

namespace condy {

template <typename T> class AsyncFutex {
public:
    AsyncFutex(std::atomic_ref<T> futex) : futex_(futex) {}

    AsyncFutex(const AsyncFutex &) = delete;
    AsyncFutex &operator=(const AsyncFutex &) = delete;
    AsyncFutex(AsyncFutex &&) = delete;
    AsyncFutex &operator=(AsyncFutex &&) = delete;

public:
    struct [[nodiscard]] WaitAwaiter;
    WaitAwaiter wait(T old) { return {*this, std::move(old)}; }

    void notify_one() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wait_awaiters_.empty()) {
            auto *handle = wait_awaiters_.pop_front();
            handle->schedule();
        }
    }

    void notify_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!wait_awaiters_.empty()) {
            auto *handle = wait_awaiters_.pop_front();
            handle->schedule();
        }
    }

private:
    class WaitFinishHandle;

    bool cancel_wait_(WaitFinishHandle *handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return wait_awaiters_.remove(handle);
    }

    bool request_wait_(WaitFinishHandle *handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto current_value = futex_.load(std::memory_order_relaxed);
        if (current_value != handle->get_old_value()) {
            return false; // No need to wait
        }
        wait_awaiters_.push_back(handle);
        detail::Context::current().runtime()->pend_work();
        return true;
    }

private:
    template <typename Handle>
    using HandleList = IntrusiveDoubleList<Handle, &Handle::link_entry_>;

    mutable std::mutex mutex_;
    HandleList<WaitFinishHandle> wait_awaiters_;
    std::atomic_ref<T> futex_;
};

template <typename T>
class AsyncFutex<T>::WaitFinishHandle
    : public InvokerAdapter<WaitFinishHandle, WorkInvoker> {
public:
    using ReturnType = bool;

    WaitFinishHandle(T old_value) : old_value_(std::move(old_value)) {}

    void cancel() {
        if (futex_->cancel_wait_(this)) {
            // Successfully canceled
            canceled_ = true;
            runtime_->resume_work();
            runtime_->schedule(this);
        }
    }

    ReturnType extract_result() {
        bool success = !canceled_;
        return success;
    }

    void set_invoker(Invoker *invoker) { invoker_ = invoker; }

    void invoke() {
        runtime_->resume_work();
        (*invoker_)();
    }

public:
    void init(AsyncFutex *futex, Runtime *runtime) {
        futex_ = futex;
        runtime_ = runtime;
    }

    T &get_old_value() { return old_value_; }

    void schedule() { runtime_->schedule(this); }

public:
    DoubleLinkEntry link_entry_;

private:
    Invoker *invoker_ = nullptr;
    AsyncFutex *futex_ = nullptr;
    Runtime *runtime_ = nullptr;
    T old_value_;
    bool canceled_ = false;
};

template <typename T> struct AsyncFutex<T>::WaitAwaiter {
public:
    using HandleType = WaitFinishHandle;

    WaitAwaiter(AsyncFutex &futex, T old_value)
        : futex_(futex), finish_handle_(std::move(old_value)) {}

public:
    HandleType *get_handle() { return &finish_handle_; }

    void init_finish_handle() { /* Leaf node, no-op */ }

    void register_operation(unsigned int /*flags*/) {
        auto *runtime = detail::Context::current().runtime();
        finish_handle_.init(&futex_, runtime);
        bool need_suspend = futex_.request_wait_(&finish_handle_);
        if (!need_suspend) {
            // No need to suspend, schedule the handle directly
            runtime->schedule(&finish_handle_);
        }
    }

public:
    bool await_ready() const noexcept { return false; }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
        init_finish_handle();
        finish_handle_.set_invoker(&h.promise());
        finish_handle_.init(&futex_, detail::Context::current().runtime());
        bool need_suspend = futex_.request_wait_(&finish_handle_);
        return need_suspend;
    }

    auto await_resume() noexcept { return finish_handle_.extract_result(); }

private:
    AsyncFutex &futex_;
    WaitFinishHandle finish_handle_;
};

} // namespace condy