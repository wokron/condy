/**
 * @file singleton.hpp
 */

#pragma once

namespace condy {

template <typename T> class ThreadLocalSingleton {
public:
    ThreadLocalSingleton() = default;
    ThreadLocalSingleton(const ThreadLocalSingleton &) = delete;
    ThreadLocalSingleton &operator=(const ThreadLocalSingleton &) = delete;
    ThreadLocalSingleton(ThreadLocalSingleton &&) = delete;
    ThreadLocalSingleton &operator=(ThreadLocalSingleton &&) = delete;

    static T &current() {
        static thread_local T instance;
        return instance;
    }
};

} // namespace condy