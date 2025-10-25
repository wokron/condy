#pragma once

#include "condy/coro.hpp"
#include "condy/semaphore.hpp"

namespace condy {

class Mutex {
public:
    auto lock() { return sem_.acquire(); }

    void unlock() { sem_.release(); }

    struct LockGuard {
        LockGuard(Mutex *m) : mutex_(m) {}
        LockGuard(LockGuard &&other) noexcept
            : mutex_(std::exchange(other.mutex_, nullptr)) {}

        LockGuard &operator=(LockGuard &&other) = delete;
        LockGuard(const LockGuard &) = delete;
        LockGuard &operator=(const LockGuard &) = delete;

        ~LockGuard() {
            if (mutex_) {
                mutex_->unlock();
            }
        }
        Mutex *mutex_;
    };

    condy::Coro<LockGuard> lock_guard() {
        co_await sem_.acquire();
        co_return LockGuard{this};
    }

private:
    SingleReleaseSemaphore sem_{1};
};

} // namespace condy