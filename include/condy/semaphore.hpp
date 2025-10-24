#pragma once

#include "condy/finish_handles.hpp"
#include "condy/queue.hpp"
#include <coroutine>
#include <cstddef>
#include <queue>

namespace condy {

// TODO: Is single-threaded version useful?
class Semaphore {
public:
    Semaphore(size_t capacity, size_t initial_count = 0)
        : capacity_(capacity), count_(initial_count) {
        assert(capacity_ > 0);
        assert(0 <= initial_count && initial_count <= capacity_);
    }

    Semaphore(const Semaphore &) = delete;
    Semaphore &operator=(const Semaphore &) = delete;
    Semaphore(Semaphore &&) = delete;
    Semaphore &operator=(Semaphore &&) = delete;

    ~Semaphore() {
        if (!wait_queue_.empty()) {
            std::terminate();
        }
    }

    struct AcquireAwaiter {
        bool await_ready() {
            if (self_.count_ > 0) {
                self_.count_--;
                return true;
            }
            return false;
        }

        void await_suspend(std::coroutine_handle<> h) {
            handle_.set_on_finish([h](int r) {
                assert(r == 0);
                h.resume();
            });
            self_.wait_queue_.emplace(&handle_);
        }

        void await_resume() {}

        Semaphore &self_;
        OpFinishHandle handle_;
    };

    AcquireAwaiter acquire() { return {.self_ = *this}; }

    void release(size_t n = 1) {
        assert(n >= 0 && count_ + n <= capacity_);
        count_ += n;
        while (count_ > 0 && !wait_queue_.empty()) {
            auto *handle = wait_queue_.front();
            wait_queue_.pop();
            bool ok = Context::current().get_ready_queue()->try_enqueue(handle);
            if (!ok) {
                auto *ring = Context::current().get_ring();
                io_uring_sqe *sqe =
                    Context::current().get_strategy()->get_sqe(ring);
                assert(sqe != nullptr);
                io_uring_prep_nop(sqe);
                io_uring_sqe_set_data(sqe, handle);
            }
            count_--;
        }
    }

    size_t capacity() const { return capacity_; }

private:
    std::queue<OpFinishHandle *> wait_queue_;
    size_t count_;
    size_t capacity_;
};

class BinarySemaphore : public Semaphore {
public:
    BinarySemaphore(size_t initial_count) : Semaphore(1, initial_count) {}
};

} // namespace condy