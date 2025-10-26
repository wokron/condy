#include "condy/awaiter_operations.hpp"
#include "condy/coro.hpp"
#include "condy/event_loop.hpp"
#include "condy/strategies.hpp"
#include <condy/task.hpp>
#include <coroutine>
#include <cstddef>
#include <doctest/doctest.h>
#include <thread>

TEST_CASE("test task - launch multiple tasks") {
    condy::EventLoop<condy::SimpleStrategy> loop(8);

    size_t unfinished = 1;

    auto sub_func = [&](int v, int &r) -> condy::Coro<void> {
        co_await condy::build_op_awaiter(io_uring_prep_nop);
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
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    loop.run(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test task - return value") {
    condy::EventLoop<condy::SimpleStrategy> loop(8);

    size_t unfinished = 1;

    auto sub_func = [](int v) -> condy::Coro<int> {
        co_await condy::build_op_awaiter(io_uring_prep_nop);
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
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    loop.run(std::move(coro));
    REQUIRE(unfinished == 0);
}

namespace {

template <typename Strategy>
class BusyEventLoop : public condy::EventLoop<Strategy> {
public:
    using Base = condy::EventLoop<Strategy>;
    using Base::Base;

    bool try_post(std::coroutine_handle<> handle) {
        if (counter_.fetch_add(1) % 10 == 9) {
            return Base::try_post(handle);
        }
        return false;
    }

private:
    std::atomic<size_t> counter_{0};
};

class BusyEventLoopStrategy : public condy::SimpleStrategy {
public:
    using Base = condy::SimpleStrategy;
    using Base::Base;

    bool should_stop() const override { return false; }
};

} // namespace

TEST_CASE("test task - co_spawn to other executor") {
    BusyEventLoop<condy::SimpleStrategy> loop{8};
    BusyEventLoop<BusyEventLoopStrategy> busy_loop{8};

    std::thread busy_loop_thread([&]() { busy_loop.run(); });

    bool task_finished = false;

    auto task_func = [&](std::thread::id prev_id) -> condy::Coro<void> {
        auto curr_id = std::this_thread::get_id();
        REQUIRE(curr_id != prev_id);
        task_finished = true;
        co_return;
    };

    loop.run(condy::Coro<void>([&]() -> condy::Coro<void> {
        auto prev_id = std::this_thread::get_id();
        auto t = co_await condy::co_spawn(busy_loop, task_func(prev_id));
        REQUIRE(t.is_remote_task());
        co_await std::move(t);
        REQUIRE(task_finished);
    }()));

    busy_loop.stop();
    busy_loop_thread.join();

    REQUIRE(task_finished);
}