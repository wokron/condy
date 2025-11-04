#pragma once

#include "condy/coro.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/task.hpp"

namespace condy {

template <typename Runtime, typename T>
T sync_wait(Runtime &runtime, Coro<T> coro) {
    auto t = co_spawn(runtime, std::move(coro));
    runtime.done();
    runtime.wait();
    return t.wait();
}

template <typename T> T sync_wait(Coro<T> coro) {
    condy::SingleThreadRuntime runtime;
    return sync_wait(runtime, std::move(coro));
}

} // namespace condy