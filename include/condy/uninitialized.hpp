#pragma once

#include <cassert>
#include <utility>

namespace condy {

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

} // namespace condy