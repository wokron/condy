#include <condy/awaiter_operations.hpp>
#include <condy/event_loop.hpp>
#include <condy/task.hpp>
#include <doctest/doctest.h>

TEST_CASE("test event_loop - basic loop") {
    condy::EventLoop<condy::SimpleStrategy> loop(8);

    auto func = [&]() -> condy::Coro<void> {
        co_await condy::build_op_awaiter(io_uring_prep_nop);
    };

    REQUIRE(loop.check_idle());

    loop.run(func());

    REQUIRE(loop.check_stopped());
}

TEST_CASE("test_event_loop - multiple tasks") {
    condy::EventLoop<condy::SimpleStrategy> loop(16);

    const int num_tasks = 10000;
    int counter = 0;

    auto task_func = [&counter]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 0,
            .tv_nsec = 100,
        };
        co_await condy::build_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        counter++;
    };
    auto func = [&]() -> condy::Coro<void> {
        std::vector<condy::Task<void>> tasks;
        tasks.reserve(num_tasks);
        for (int i = 0; i < num_tasks; ++i) {
            tasks.emplace_back(condy::co_spawn(task_func()));
        }
        for (auto &&t : std::move(tasks)) {
            co_await std::move(t);
        }
    };

    REQUIRE(loop.check_idle());

    loop.run(func());

    REQUIRE(loop.check_stopped());
    REQUIRE(counter == num_tasks);
}