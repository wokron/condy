#pragma once

#include <atomic>

namespace condy {

// TODO: Optimize this SpinLock
class SpinLock {
public:
    SpinLock() = default;
    ~SpinLock() = default;

    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire))
            ; // Busy wait
    }

    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

} // namespace condy