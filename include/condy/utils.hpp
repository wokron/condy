#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <system_error>
#include <utility>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#else
#define __tsan_acquire(addr) static_cast<void>(0)
#define __tsan_release(addr) static_cast<void>(0)
#endif
#else
#define __tsan_acquire(addr) static_cast<void>(0)
#define __tsan_release(addr) static_cast<void>(0)
#endif

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

inline void panic_on(const char *msg) noexcept {
    std::cerr << "Panic: " << msg << std::endl;
#ifndef CRASH_TEST
    std::terminate();
#else
    // Ctest cannot handle SIGABRT, so we use exit here
    std::exit(EXIT_FAILURE);
#endif
}

template <typename T> class RawStorage {
public:
    template <typename... Args> void construct(Args &&...args) {
        new (&storage_) T(std::forward<Args>(args)...);
    }

    T &get() { return *reinterpret_cast<T *>(&storage_); }

    const T &get() const { return *reinterpret_cast<const T *>(&storage_); }

    void destroy() { get().~T(); }

private:
    alignas(T) unsigned char storage_[sizeof(T)];
};

template <typename T, size_t N> class SmallArray {
public:
    SmallArray(size_t capacity) : capacity_(capacity) {
        if (!is_small_()) {
            large_ = new T[capacity];
        }
    }

    ~SmallArray() {
        if (!is_small_()) {
            delete[] large_;
        }
    }

    T &operator[](size_t index) {
        return is_small_() ? small_[index] : large_[index];
    }

    const T &operator[](size_t index) const {
        return is_small_() ? small_[index] : large_[index];
    }

    size_t capacity() const { return capacity_; }

private:
    bool is_small_() const { return capacity_ <= N; }

private:
    size_t capacity_;
    union {
        T small_[N];
        T *large_;
    };
};

// TODO: Remove this after channel refactor
template <typename T> class Uninitialized {
public:
    Uninitialized() = default;
    ~Uninitialized() {
        if (initialized_) {
            get().~T();
        }
    }

    template <typename... Args> void emplace(Args &&...args) {
        assert(!initialized_ && "Object is already initialized");
        new (&storage_) T(std::forward<Args>(args)...);
        initialized_ = true;
    }

    T &get() { return *reinterpret_cast<T *>(&storage_); }

    const T &get() const { return *reinterpret_cast<const T *>(&storage_); }

    void reset() {
        if (initialized_) {
            get().~T();
            initialized_ = false;
        }
    }

private:
    alignas(T) unsigned char storage_[sizeof(T)];
    bool initialized_ = false;
};

inline auto make_system_error(const char *msg, int ec) {
    return std::system_error(ec, std::generic_category(), msg);
}

template <typename M, typename T> constexpr ptrdiff_t offset_of(M T::*member) {
    constexpr T *dummy = nullptr;
    return reinterpret_cast<ptrdiff_t>(&(dummy->*member));
}

template <typename M, typename T> T *container_of(M T::*member, M *ptr) {
    auto offset = offset_of(member);
    return reinterpret_cast<T *>(reinterpret_cast<intptr_t>(ptr) - offset);
}

template <typename T> constexpr bool is_power_of_two(T n) {
    return n > 0 && (n & (n - 1)) == 0;
}

} // namespace condy
