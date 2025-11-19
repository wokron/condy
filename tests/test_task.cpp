#include "condy/awaiter_operations.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/task.hpp"
#include <chrono>
#include <doctest/doctest.h>
#include <thread>

namespace {

condy::RuntimeOptions options = condy::RuntimeOptions().sq_size(8).cq_size(16);

} // namespace

TEST_CASE("test task - local spawn and await") {
    condy::Runtime runtime(options);
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
    runtime.run();

    REQUIRE(finished);
}

TEST_CASE("test task - remote spawn and await") {
    condy::Runtime runtime1(options), runtime2(options);
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

    std::thread rt2_thread([&]() { runtime2.run(); });

    runtime2.block_until_running();

    runtime1.schedule(&h.promise());
    runtime1.done();
    runtime1.run();

    runtime2.done();
    rt2_thread.join();

    REQUIRE(finished);
}

TEST_CASE("test task - remote spawn and wait 1") {
    condy::Runtime runtime(options);
    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        finished = true;
        co_return;
    };

    auto task = condy::co_spawn(runtime, func());

    runtime.done();
    runtime.run();

    REQUIRE(finished);

    task.wait();
}

TEST_CASE("test task - remote spawn and wait 2") {
    condy::Runtime runtime(options);
    bool finished = false;

    auto func = [&]() -> condy::Coro<void> {
        finished = true;
        co_return;
    };

    std::thread rt_thread([&]() { runtime.run(); });

    runtime.block_until_running();

    auto task = condy::co_spawn(runtime, func());
    task.wait();

    REQUIRE(finished);

    runtime.done();
    rt_thread.join();
}

TEST_CASE("test task - launch multiple tasks") {
    condy::Runtime runtime(options);

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
    runtime.run();

    REQUIRE(finished);
}

TEST_CASE("test task - return value") {
    condy::Runtime runtime(options);

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
    runtime.run();

    REQUIRE(finished);
}

TEST_CASE("test task - return value with wait") {
    condy::Runtime runtime(options);

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

    std::thread rt_thread([&]() { runtime.run(); });

    runtime.block_until_running();

    auto task = condy::co_spawn(runtime, func());
    int r = task.wait();
    REQUIRE(r == 42);
    REQUIRE(finished);

    runtime.done();
    rt_thread.join();
}

TEST_CASE("test task - exception propagation") {
    condy::Runtime runtime(options);

    auto func = [&]() -> condy::Coro<void> {
        co_await condy::make_op_awaiter(io_uring_prep_nop);
        throw std::runtime_error("Test exception");
    };

    auto main = [&]() -> condy::Coro<void> {
        auto task = condy::co_spawn(func());
        co_await std::move(task);
    };

    std::thread rt_thread([&]() { runtime.run(); });

    runtime.block_until_running();

    auto task = condy::co_spawn(runtime, main());
    REQUIRE_THROWS_AS(task.wait(), std::runtime_error);

    runtime.done();
    rt_thread.join();
}

TEST_CASE("test task - run in different thread") {
    condy::Runtime runtime1(options), runtime2(options);

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
        auto t2 = condy::co_spawn(runtime1, local(prev_id));
        co_await std::move(t2);
        REQUIRE(finished2);
        co_await std::move(t1);
        REQUIRE(finished1);
        task_finished = true;
    };

    std::thread rt2_thread([&]() { runtime2.run(); });

    runtime2.block_until_running();

    condy::co_spawn(runtime1, main()).detach();

    runtime1.done();
    runtime1.run();

    runtime2.done();
    rt2_thread.join();

    REQUIRE(finished1);
    REQUIRE(finished2);
    REQUIRE(task_finished);
}

TEST_CASE("test task - detach") {
    condy::Runtime runtime(options);

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
    runtime.run();

    REQUIRE(finished);
}

TEST_CASE("test task - spawn task with custom allocator") {
    struct CustomAllocator {
        using value_type = char;
        value_type *allocate(size_t size) {
            allocated_size = size;
            allocated = true;
            return reinterpret_cast<value_type *>(::malloc(size));
        }
        void deallocate(value_type *ptr, size_t size) {
            REQUIRE(size == allocated_size);
            ::free(ptr);
        }
        size_t allocated_size = 0;
        bool allocated = false;
    };

    CustomAllocator allocator;

    condy::Runtime runtime(options);
    bool finished = false;

    auto func = [&](auto &) -> condy::Coro<void, CustomAllocator> {
        finished = true;
        co_return;
    };

    auto main = [&]() -> condy::Coro<void> {
        auto task = condy::co_spawn(func(allocator));
        co_await std::move(task);
    };

    auto coro = main();
    auto h = coro.release();

    runtime.schedule(&h.promise());
    runtime.done();
    runtime.run();

    REQUIRE(finished);
    REQUIRE(allocator.allocated);
}

TEST_CASE("test task - co_switch") {
    condy::Runtime runtime1(options), runtime2(options);

    bool finished1 = false, finished2 = false, task_finished = false;

    auto func = [&]() -> condy::Coro<void> {
        auto id1 = std::this_thread::get_id();
        co_await condy::co_switch(runtime2);
        finished1 = true;
        auto id2 = std::this_thread::get_id();
        REQUIRE(id1 != id2);
        co_await condy::co_switch(runtime1);
        finished2 = true;
        auto id3 = std::this_thread::get_id();
        REQUIRE(id2 != id3);
        REQUIRE(id1 == id3);

        co_await condy::co_switch(runtime2);
        co_return;
    };

    auto main = [&]() -> condy::Coro<void> {
        auto prev_id = std::this_thread::get_id();
        co_await condy::co_spawn(func());
        REQUIRE(finished1);
        REQUIRE(finished2);
        task_finished = true;
    };

    std::thread rt2_thread([&]() { runtime2.run(); });

    runtime2.block_until_running();

    condy::co_spawn(runtime1, main()).detach();

    runtime1.done();
    runtime1.run();

    runtime2.done();
    rt2_thread.join();

    REQUIRE(task_finished);
}