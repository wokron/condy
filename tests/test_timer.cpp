#include "condy/runtime.hpp"
#include "condy/timer.hpp"
#include <doctest/doctest.h>

TEST_CASE("test timer - sleep") {
    condy::SingleThreadRuntime runtime;

    condy::Timer timer;

    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{.tv_sec = 0, .tv_nsec = 10000};
        auto start = std::chrono::steady_clock::now();
        auto r = co_await timer.async_wait(&ts, 0, 0);
        REQUIRE(r == -ETIME);
        auto end = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count();
        REQUIRE(duration < 100); // Less than 20 microseconds
        co_return;
    };

    auto t = condy::co_spawn(runtime, func());

    runtime.done();
    runtime.wait();
    t.wait();
}

TEST_CASE("test timer - sleep with cancel") {
    condy::SingleThreadRuntime runtime;

    condy::Timer timer;

    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{.tv_sec = 60 * 60, .tv_nsec = 0};
        auto start = std::chrono::steady_clock::now();
        int r = co_await timer.async_wait(&ts, 0, 0);
        REQUIRE(r == -ECANCELED);
        auto end = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                .count();
        REQUIRE(duration < 100); // Should be canceled quickly
        co_return;
    };

    auto canceller = [&]() -> condy::Coro<void> {
        auto r = co_await timer.async_remove(0);
        REQUIRE(r == 0);
        co_return;
    };

    // Single thread, so we can be sure that canceller runs after func
    auto t1 = condy::co_spawn(runtime, func());
    auto t2 = condy::co_spawn(runtime, canceller());

    runtime.done();
    runtime.wait();
    t1.wait();
    t2.wait();
}

TEST_CASE("test timer - sleep with update") {
    condy::SingleThreadRuntime runtime;

    condy::Timer timer;

    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{.tv_sec = 60 * 60, .tv_nsec = 0};
        auto start = std::chrono::steady_clock::now();
        int r = co_await timer.async_wait(&ts, 0, 0);
        REQUIRE(r == -ETIME);
        auto end = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                .count();
        REQUIRE(duration < 100);  // Less than 20 milliseconds
        co_return;
    };

    auto updater = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{.tv_sec = 0, .tv_nsec = 10000};
        auto r = co_await timer.async_update(&ts, 0);
        REQUIRE(r == 0);
        co_return;
    };

    // Single thread, so we can be sure that updater runs after func
    auto t1 = condy::co_spawn(runtime, func());
    auto t2 = condy::co_spawn(runtime, updater());

    runtime.done();
    runtime.wait();
    t1.wait();
    t2.wait();
}