#pragma once

#include "condy/event_loop.hpp"
#include "condy/link_list.hpp"
#include "condy/mutex.hpp"
#include "condy/semaphore.hpp"
#include "condy/spin_lock.hpp"

namespace condy {

class ConditionVariable {
public:
    ConditionVariable(Mutex &mutex) : mutex_(mutex) {}

    ConditionVariable(const ConditionVariable &) = delete;
    ConditionVariable &operator=(const ConditionVariable &) = delete;
    ConditionVariable(ConditionVariable &&) = delete;
    ConditionVariable &operator=(ConditionVariable &&) = delete;

    ~ConditionVariable() = default;

    void notify_one() {
        std::lock_guard lock(lock_);
        auto *awaiter = static_cast<WaitAwaiter *>(wait_queue_.try_pop());
        if (awaiter == nullptr) {
            return;
        }
        wakeup_one_(awaiter);
    }

    void notify_all() {
        std::lock_guard lock(lock_);
        wait_queue_.pop_all([&](IntrusiveNode *node) {
            wakeup_one_(static_cast<WaitAwaiter *>(node));
        });
    }

    using AcquireAwaiter = SingleReleaseSemaphore::AcquireAwaiter;

    struct [[nodiscard]] WaitAwaiter : public AcquireAwaiter {
        WaitAwaiter(ConditionVariable &cv, SingleReleaseSemaphore &sem,
                    IEventLoop *event_loop)
            : AcquireAwaiter(sem, event_loop), cv_(cv) {}

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept;
        void await_resume() const noexcept {}

        ConditionVariable &cv_;
    };

    WaitAwaiter wait() {
        return {*this, mutex_.sem_, Context::current().get_event_loop()};
    }

    template <typename Func> Coro<void> wait(Func &&pred) {
        while (!pred()) {
            co_await wait();
        }
    }

private:
    void wakeup_one_(WaitAwaiter *awaiter) {
        auto *acquire_awaiter =
            static_cast<SingleReleaseSemaphore::AcquireAwaiter *>(awaiter);
        if (acquire_awaiter->await_ready()) {
            auto event_loop = awaiter->event_loop_;
            while (!event_loop->try_post(awaiter->handle_))
                ; // Busy wait // TODO: improve this
        } else {
            acquire_awaiter->await_suspend(acquire_awaiter->handle_);
        }
    }

private:
    SpinLock lock_;
    LinkList wait_queue_;
    Mutex &mutex_;
};

inline void ConditionVariable::WaitAwaiter::await_suspend(
    std::coroutine_handle<> h) noexcept {
    handle_ = h;
    cv_.wait_queue_.push(this);
    sem_.release(); // Unlock after pushed to avoid lost wakeup
}

} // namespace condy