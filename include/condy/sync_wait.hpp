#pragma once

#include "condy/coro.hpp"
#include "condy/runtime.hpp"
#include "condy/task.hpp"

namespace condy {

template <typename T, typename Allocator>
T sync_wait(Runtime &runtime, Coro<T, Allocator> coro) {
    auto t = co_spawn(runtime, std::move(coro));
    runtime.done();
    try {
        runtime.run();
    } catch (...) {
        t.detach();
        throw;
    }
    return t.wait();
}

inline static RuntimeOptions &default_runtime_options() {
    static RuntimeOptions options;
    return options;
}

template <typename T, typename Allocator> T sync_wait(Coro<T, Allocator> coro) {
    condy::Runtime runtime(default_runtime_options());
    return sync_wait(runtime, std::move(coro));
}

} // namespace condy