#include "condy/awaiter_operations.hpp"
#include "condy/runtime.hpp"
#include "condy/task.hpp"
#include <doctest/doctest.h>
#include <thread>

TEST_CASE("test task - local spawn and await") {
    condy::SingleThreadRuntime runtime(8);
    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        finished = true;
        co_return;
    };

    auto main = [&]() -> condy::Coro<void> {
        auto task = condy::co_spawn(func());
        co_await std::move(task);
    };

    auto coro = main();
    auto h = coro.release();

    runtime.schedule(&h.promise());
    runtime.done();
    runtime.wait();

    REQUIRE(finished);
}

TEST_CASE("test task - remote spawn and await") {
    condy::SingleThreadRuntime runtime1(8), runtime2(8);
    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        finished = true;
        co_return;
    };

    auto main = [&]() -> condy::Coro<void> {
        auto task = condy::co_spawn(runtime2, func());
        co_await std::move(task);
    };

    auto coro = main();
    auto h = coro.release();

    std::thread rt2_thread([&]() { runtime2.wait(); });

    runtime1.schedule(&h.promise());
    runtime1.done();
    runtime1.wait();

    runtime2.done();
    rt2_thread.join();

    REQUIRE(finished);
}

TEST_CASE("test task - remote spawn and wait 1") {
    condy::SingleThreadRuntime runtime(8);
    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        finished = true;
        co_return;
    };

    auto task = condy::co_spawn(runtime, func());

    runtime.done();
    runtime.wait();

    REQUIRE(finished);

    task.wait();
}

TEST_CASE("test task - remote spawn and wait 2") {
    condy::SingleThreadRuntime runtime(8);
    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        finished = true;
        co_return;
    };

    std::thread rt_thread([&]() { runtime.wait(); });

    auto task = condy::co_spawn(runtime, func());
    task.wait();

    REQUIRE(finished);

    runtime.done();
    rt_thread.join();
}

TEST_CASE("test task - launch multiple tasks") {
    condy::SingleThreadRuntime runtime(8);

    bool finished = false;

    auto sub_func = [&](int v, int &r) -> condy::Coro<void> {
        co_await condy::make_op_awaiter(io_uring_prep_nop);
        r = v;
    };

    auto func = [&]() -> condy::Coro<void> {
        int r1 = 0, r2 = 0, r3 = 0;
        auto t1 = condy::co_spawn(sub_func(1, r1));
        auto t2 = condy::co_spawn(sub_func(2, r2));
        auto t3 = condy::co_spawn(sub_func(3, r3));
        co_await std::move(t3);
        REQUIRE(r3 == 3);
        co_await std::move(t2);
        REQUIRE(r2 == 2);
        co_await std::move(t1);
        REQUIRE(r1 == 1);
        finished = true;
    };

    condy::co_spawn(runtime, func()).detach();

    runtime.done();
    runtime.wait();

    REQUIRE(finished);
}

TEST_CASE("test task - return value") {
    condy::SingleThreadRuntime runtime(8);

    bool finished = false;

    auto sub_func = [](int v) -> condy::Coro<int> {
        co_await condy::make_op_awaiter(io_uring_prep_nop);
        co_return v;
    };

    auto func = [&]() -> condy::Coro<void> {
        auto t1 = condy::co_spawn(sub_func(10));
        auto t2 = condy::co_spawn(sub_func(20));
        auto t3 = condy::co_spawn(sub_func(30));
        int r3 = co_await std::move(t3);
        REQUIRE(r3 == 30);
        int r2 = co_await std::move(t2);
        REQUIRE(r2 == 20);
        int r1 = co_await std::move(t1);
        REQUIRE(r1 == 10);
        finished = true;
    };

    condy::co_spawn(runtime, func()).detach();

    runtime.done();
    runtime.wait();

    REQUIRE(finished);
}

TEST_CASE("test task - return value with wait") {
    condy::SingleThreadRuntime runtime(8);

    bool finished = false;

    auto func = [&]() -> condy::Coro<int> {
        __kernel_timespec ts{
            .tv_sec = 0,
            .tv_nsec = 1000000, // 1ms
        };
        co_await condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        finished = true;
        co_return 42;
    };

    std::thread rt_thread([&]() { runtime.wait(); });

    auto task = condy::co_spawn(runtime, func());
    int r = task.wait();
    REQUIRE(r == 42);
    REQUIRE(finished);

    runtime.done();
    rt_thread.join();
}

TEST_CASE("test task - exception propagation") {
    condy::SingleThreadRuntime runtime(8);

    auto func = [&]() -> condy::Coro<void> {
        co_await condy::make_op_awaiter(io_uring_prep_nop);
        throw std::runtime_error("Test exception");
    };

    auto main = [&]() -> condy::Coro<void> {
        auto task = condy::co_spawn(func());
        co_await std::move(task);
    };

    std::thread rt_thread([&]() { runtime.wait(); });

    auto task = condy::co_spawn(runtime, main());
    REQUIRE_THROWS_AS(task.wait(), std::runtime_error);

    runtime.done();
    rt_thread.join();
}

TEST_CASE("test task - run in different thread") {
    condy::SingleThreadRuntime runtime1(8), runtime2(8);

    bool finished1 = false, finished2 = false, task_finished = false;

    auto remote = [&](std::thread::id prev_id) -> condy::Coro<void> {
        auto curr_id = std::this_thread::get_id();
        REQUIRE(curr_id != prev_id);
        finished1 = true;
        co_return;
    };

    auto local = [&](std::thread::id prev_id) -> condy::Coro<void> {
        auto curr_id = std::this_thread::get_id();
        REQUIRE(curr_id == prev_id);
        finished2 = true;
        co_return;
    };

    auto main = [&]() -> condy::Coro<void> {
        auto prev_id = std::this_thread::get_id();
        auto t1 = condy::co_spawn(runtime2, remote(prev_id));
        REQUIRE(t1.is_remote_task());
        auto t2 = condy::co_spawn(runtime1, local(prev_id));
        REQUIRE(!t2.is_remote_task());
        co_await std::move(t2);
        REQUIRE(finished2);
        co_await std::move(t1);
        REQUIRE(finished1);
        task_finished = true;
    };

    std::thread rt2_thread([&]() { runtime2.wait(); });

    condy::co_spawn(runtime1, main()).detach();

    runtime1.done();
    runtime1.wait();

    runtime2.done();
    rt2_thread.join();

    REQUIRE(finished1);
    REQUIRE(finished2);
    REQUIRE(task_finished);
}

TEST_CASE("test task - detach") {
    condy::SingleThreadRuntime runtime(8);

    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        co_await condy::make_op_awaiter(io_uring_prep_nop);
        finished = true;
        co_return;
    };

    auto main = [&]() -> condy::Coro<void> {
        auto task = condy::co_spawn(func());
        task.detach();
        co_return;
    };

    condy::co_spawn(runtime, main()).detach();

    runtime.done();
    runtime.wait();

    REQUIRE(finished);
}

TEST_CASE("test task - spawn in multi thread runtime") {
    condy::MultiThreadRuntime runtime(8, 4);

    bool finished = false;

    auto func1 = [&](std::thread::id prev_id) -> condy::Coro<void> {
        REQUIRE(std::this_thread::get_id() != prev_id);
        co_await condy::make_op_awaiter(io_uring_prep_nop);
        finished = true;
    };

    auto func2 = [&]() -> condy::Coro<void> {
        auto curr_id = std::this_thread::get_id();
        auto t = condy::co_spawn(func1(curr_id));
        REQUIRE(!t.is_remote_task());
        REQUIRE(!t.is_single_thread_task());
        t.wait(); // Block current thread, so task must run in another thread

        REQUIRE(finished);
        co_return;
    };

    condy::co_spawn(runtime, func2()).detach();

    runtime.done();
    runtime.wait();

    REQUIRE(finished);
}