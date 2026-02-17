/**
 * @file sync_wait.hpp
 * @brief Helper functions for running a Runtime.
 */

#pragma once

#include "condy/coro.hpp"
#include "condy/runtime.hpp"
#include "condy/task.hpp"

namespace condy {

/**
 * @brief Synchronously wait for a coroutine to complete in the given runtime.
 * @tparam T Type of the coroutine result.
 * @tparam Allocator Allocator type used for memory management.
 * @param runtime The runtime to run the coroutine in.
 * @param coro The coroutine to be run.
 * @return T The result of the coroutine.
 * @note This function will exit after all coroutines are completed.
 */
template <typename T, typename Allocator>
T sync_wait(Runtime &runtime, Coro<T, Allocator> coro) {
    auto t = co_spawn(runtime, std::move(coro));
    runtime.allow_exit();
    runtime.run();
    return t.wait();
}

/**
 * @brief Get the default runtime options. This options will be used when
 * using sync_wait without specifying runtime.
 * @return RuntimeOptions& Reference to the default runtime options.
 */
inline RuntimeOptions &default_runtime_options() {
    static RuntimeOptions options;
    return options;
}

/**
 * @brief Synchronously wait for a coroutine to complete using a default
 * runtime. The runtime will be created and destroyed for each call.
 * @tparam T Type of the coroutine result.
 * @tparam Allocator Allocator type used for memory management.
 * @param coro The coroutine to be run.
 * @return T The result of the coroutine.
 * @note This function will exit after all coroutines are completed.
 */
template <typename T, typename Allocator> T sync_wait(Coro<T, Allocator> coro) {
    condy::Runtime runtime(default_runtime_options());
    return sync_wait(runtime, std::move(coro));
}

} // namespace condy