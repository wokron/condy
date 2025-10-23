#include "condy/awaiter_operations.hpp"
#include "condy/coro.hpp"
#include "condy/event_loop.hpp"
#include "condy/finish_handles.hpp"
#include <condy/task.hpp>
#include <cstddef>
#include <doctest/doctest.h>
#include <semaphore>
#include <thread>

TEST_CASE("test task - launch multiple tasks") {
    condy::EventLoop loop(std::make_unique<condy::SimpleStrategy>(8));

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
    condy::EventLoop loop(std::make_unique<condy::SimpleStrategy>(8));

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

class SimpleExecutor {
public:
    bool try_post(condy::OpFinishHandle *handle) {
        // Use counter to simulate busy executor
        if (--counter_ == 0) {
            handle_ = handle;
            sem_.release();
            return true;
        }
        return false;
    }

    void run() {
        sem_.acquire();
        handle_->finish(0);
        sem_.release();
    }

private:
    condy::OpFinishHandle *handle_;
    std::binary_semaphore sem_{0};
    size_t counter_ = 10;
};

} // namespace

TEST_CASE("test task - co_spawn to other executor") {
    condy::EventLoop loop(std::make_unique<condy::SimpleStrategy>(8));
    SimpleExecutor executor;

    std::thread executor_thread([&]() { executor.run(); });

    std::atomic<bool> task_finished = false;

    auto task_func = [&](std::thread::id prev_id) -> condy::Coro<void> {
        auto curr_id = std::this_thread::get_id();
        REQUIRE(curr_id != prev_id);
        task_finished = true;
        co_return;
    };

    loop.run(condy::Coro<void>([&]() -> condy::Coro<void> {
        auto prev_id = std::this_thread::get_id();
        co_await condy::co_spawn(executor, task_func(prev_id));
        while (!task_finished.load()) {
            co_await condy::build_op_awaiter(io_uring_prep_nop);
        }
    }()));

    executor_thread.join();

    REQUIRE(task_finished.load());
}