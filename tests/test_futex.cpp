#include "condy/coro.hpp"
#include "condy/futex.hpp"
#include "condy/sync_wait.hpp"
#include "condy/task.hpp"
#include <atomic>
#include <doctest/doctest.h>

TEST_CASE("test async_futex - basic wait and notify") {
    std::atomic<int> atomic_counter(0);
    std::atomic_ref<int> ref_to_atomic(reinterpret_cast<int &>(atomic_counter));
    condy::AsyncFutex<int> futex(ref_to_atomic);

    bool finished = false;

    auto wake_func = [&]() -> condy::Coro<void> {
        REQUIRE(!finished);
        REQUIRE(atomic_counter.load() == 0);
        atomic_counter++;
        futex.notify_one();
        co_return;
    };

    auto wait_task = [&]() -> condy::Coro<void> {
        auto t = condy::co_spawn(wake_func());
        co_await futex.wait(0);
        REQUIRE(atomic_counter.load() == 1);
        finished = true;
        co_await t;
    };

    condy::sync_wait(wait_task());
    REQUIRE(finished);
}