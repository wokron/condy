#pragma once

#include <functional>
#include <utility>

namespace condy {

class Defer {
public:
    template <typename Func>
    Defer(Func &&func) : func_(std::forward<Func>(func)) {}
    ~Defer() { func_(); }

    Defer(const Defer &) = delete;
    Defer &operator=(const Defer &) = delete;
    Defer(Defer &&) = delete;
    Defer &operator=(Defer &&) = delete;

private:
    std::function<void()> func_;
};

template <typename Func> Defer defer(Func &&func) {
    return Defer(std::forward<Func>(func));
}

template <typename BaseMutex> class MaybeMutex : public BaseMutex {
public:
    using Base = BaseMutex;
    using Base::Base;

    void lock() noexcept {
        if (use_mutex_) {
            Base::lock();
        }
    }

    void unlock() noexcept {
        if (use_mutex_) {
            Base::unlock();
        }
    }

    bool try_lock() noexcept {
        if (use_mutex_) {
            return Base::try_lock();
        }
        return true;
    }

    void set_use_mutex(bool use_mutex) noexcept { use_mutex_ = use_mutex; }

private:
    bool use_mutex_ = false;
};

} // namespace condy