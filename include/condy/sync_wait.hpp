#pragma once

#include "condy/coro.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/task.hpp"

namespace condy {

template <typename Runtime, typename T, typename Allocator>
T sync_wait(Runtime &runtime, Coro<T, Allocator> coro) {
    auto t = co_spawn(runtime, std::move(coro));
    runtime.done();
    runtime.wait();
    return t.wait();
}

template <typename T, typename Allocator> T sync_wait(Coro<T, Allocator> coro) {
    condy::Runtime runtime;
    return sync_wait(runtime, std::move(coro));
}

} // namespace condy