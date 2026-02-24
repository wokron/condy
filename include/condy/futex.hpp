/**
 * @file futex.hpp
 * @brief User-space "futex" implementation for efficient synchronization
 * between coroutines.
 */

#pragma once

#include "condy/intrusive.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include <atomic>

namespace condy {

/**
 * @brief User-space "futex" implementation for efficient synchronization
 * between coroutines.
 * @details This class provides a user-space futex implementation that allows
 * coroutines to wait on a futex value and be efficiently notified when the
 * value changes. This class is different from condy::async_futex_wait()
 * which can be used together with thread-based synchronous wait.
 * @tparam T Type of the futex value.
 */
template <typename T> class AsyncFutex {
public:
    AsyncFutex(std::atomic_ref<T> futex) : futex_(futex) {}

    AsyncFutex(const AsyncFutex &) = delete;
    AsyncFutex &operator=(const AsyncFutex &) = delete;
    AsyncFutex(AsyncFutex &&) = delete;
    AsyncFutex &operator=(AsyncFutex &&) = delete;

public:
    struct [[nodiscard]] WaitAwaiter;
    /**
     * @brief Wait if the futex value equals to the specified old value. The
     * awaiting coroutine will be suspended until a notify is received. If the
     * value of the futex is not equal to the old value, the awaiting coroutine
     * will not be suspended.
     * @param old The old value to compare with the futex value.
     * @return WaitAwaiter Awaiter object for the wait operation.
     */
    WaitAwaiter wait(T old) noexcept { return {*this, std::move(old)}; }

    /**
     * @brief Notify one awaiting coroutine, if any.
     * @note This function is thread-safe and can be called from any thread.
     */
    void notify_one() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto *handle = wait_awaiters_.pop_front();
        if (handle) {
            handle->schedule();
        }
    }

    /**
     * @brief Notify all awaiting coroutines.
     * @note This function is thread-safe and can be called from any thread.
     */
    void notify_all() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        while (auto *handle = wait_awaiters_.pop_front()) {
            handle->schedule();
        }
    }

private:
    class WaitFinishHandle;

    bool cancel_wait_(WaitFinishHandle *handle) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return wait_awaiters_.remove(handle);
    }

    // Returns true if the caller needs to suspend
    bool request_wait_(WaitFinishHandle *handle) noexcept {
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

    void cancel() noexcept {
        if (futex_->cancel_wait_(this)) {
            // Successfully canceled
            canceled_ = true;
            runtime_->resume_work();
            runtime_->schedule(this);
        }
    }

    ReturnType extract_result() noexcept {
        bool success = !canceled_;
        return success;
    }

    void set_invoker(Invoker *invoker) noexcept { invoker_ = invoker; }

    void invoke() noexcept {
        if (need_resume_) {
            runtime_->resume_work();
        }
        (*invoker_)();
    }

public:
    void init(AsyncFutex *futex, Runtime *runtime) noexcept {
        futex_ = futex;
        runtime_ = runtime;
    }

    T &get_old_value() noexcept { return old_value_; }

    void schedule() noexcept {
        need_resume_ = true;
        runtime_->schedule(this);
    }

public:
    DoubleLinkEntry link_entry_;

private:
    Invoker *invoker_ = nullptr;
    AsyncFutex *futex_ = nullptr;
    Runtime *runtime_ = nullptr;
    T old_value_;
    bool need_resume_ = false;
    bool canceled_ = false;
};

/**
 * @brief Awaiter for waiting on the futex.
 * @return true if the wait operation is successful, false if the wait operation
 * is canceled.
 */
template <typename T> struct AsyncFutex<T>::WaitAwaiter {
public:
    using HandleType = WaitFinishHandle;

    WaitAwaiter(AsyncFutex &futex, T old_value)
        : futex_(futex), finish_handle_(std::move(old_value)) {}

public:
    HandleType *get_handle() noexcept { return &finish_handle_; }

    void init_finish_handle() noexcept { /* Leaf node, no-op */ }

    void register_operation(unsigned int /*flags*/) noexcept {
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