#pragma once

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#else
extern "C" {
inline void __tsan_acquire(void *addr) { (void)addr; }
inline void __tsan_release(void *addr) { (void)addr; }
}
#endif
#else
extern "C" {
inline void __tsan_acquire(void *addr) { (void)addr; }
inline void __tsan_release(void *addr) { (void)addr; }
}
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

private:
    bool initialized_ = false;
    alignas(T) unsigned char storage_[sizeof(T)];
};

template <typename ExceptionType = std::runtime_error>
[[noreturn]] inline void throw_exception(const char *msg, int error_code) {
    std::string full_msg;
    full_msg += msg;
    full_msg += ": ";
    full_msg += std::strerror(error_code);
    throw ExceptionType(full_msg);
}

template <typename ExceptionType = std::runtime_error>
[[noreturn]] inline void throw_exception(const char *msg) {
    int error_code = errno;
    throw_exception<ExceptionType>(msg, error_code);
}

} // namespace condy
