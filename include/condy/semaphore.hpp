// #pragma once

// #include "condy/event_loop.hpp"
// #include "condy/link_list.hpp"
// #include "condy/spin_lock.hpp"
// #include <atomic>
// #include <coroutine>
// #include <cstdint>
// #include <sys/types.h>

// namespace condy {

// class SingleReleaseSemaphore {
// public:
//     SingleReleaseSemaphore(ssize_t desired) : count_(desired) {}

//     struct [[nodiscard]] AcquireAwaiter : public IntrusiveNode {
//         AcquireAwaiter(SingleReleaseSemaphore &self, IEventLoop *event_loop)
//             : sem_(self), event_loop_(event_loop) {}
//         bool await_ready() noexcept;
//         template <typename PromiseType>
//         void await_suspend(std::coroutine_handle<PromiseType> h) noexcept;
//         void await_resume() noexcept {}

//         void await_suspend(Invoker *invoker) noexcept {
//             invoker_ = invoker;
//             sem_.wait_queue_.push(this);
//         }

//         SingleReleaseSemaphore &sem_;
//         IEventLoop *event_loop_ = nullptr;
//         Invoker *invoker_ = nullptr;
//     };

//     AcquireAwaiter acquire() {
//         return {*this, Context::current().get_event_loop()};
//     }

//     void release(ssize_t n = 1) {
//         assert(n >= 0);
//         ssize_t old_count = count_.fetch_add(n, std::memory_order_release);
//         if (old_count >= 0) {
//             return;
//         }
//         ssize_t to_wake = std::min(n, -old_count);
//         for (ssize_t i = 0; i < to_wake; ++i) {
//             AcquireAwaiter *awaiter;
//             while ((awaiter = wait_queue_.try_pop()) == nullptr)
//                 ; // Busy wait

//             auto event_loop = awaiter->event_loop_;
//             // TODO: if (event_loop == Context::current().get_event_loop()) ...
//             while (!event_loop->try_post(awaiter->invoker_))
//                 ; // Busy wait // TODO: improve this
//         }
//     }

// private:
//     std::atomic<ssize_t> count_ = 0;
//     LinkList<AcquireAwaiter> wait_queue_;
// };

// inline bool SingleReleaseSemaphore::AcquireAwaiter::await_ready() noexcept {
//     int64_t old_count = sem_.count_.fetch_sub(1, std::memory_order_acquire);
//     return old_count > 0;
// }

// template <typename PromiseType>
// void SingleReleaseSemaphore::AcquireAwaiter::await_suspend(
//     std::coroutine_handle<PromiseType> h) noexcept {
//     invoker_ = &h.promise();
//     sem_.wait_queue_.push(this);
// }

// class Semaphore : public SingleReleaseSemaphore {
// public:
//     using Base = SingleReleaseSemaphore;
//     using Base::Base;

//     void release(ssize_t n = 1) {
//         std::lock_guard lock(mutex_);
//         Base::release(n);
//     }

// private:
//     SpinLock mutex_;
// };

// } // namespace condy