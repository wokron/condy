#include "condy/awaiter_operations.hpp"
#include "condy/coro.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include "condy/task.hpp"
#include <atomic>
#include <doctest/doctest.h>
#include <thread>

namespace {

struct SetFinishInvoker
    : public condy::InvokerAdapter<SetFinishInvoker, condy::WorkInvoker> {
    void invoke() { finished = true; }
    bool finished = false;
};

condy::RuntimeOptions options = condy::RuntimeOptions().sq_size(8).cq_size(16);

} // namespace

TEST_CASE("test runtime - single thread no-op") {
    condy::Runtime runtime(options);
    runtime.allow_exit();
    runtime.run(); // Should exit immediately
}

TEST_CASE("test runtime - single thread schedule no-op") {
    condy::Runtime runtime(options);

    SetFinishInvoker invoker;
    runtime.schedule(&invoker);
    runtime.allow_exit();
    runtime.run();

    REQUIRE(invoker.finished);
}

TEST_CASE("test runtime - single thread schedule multiple no-op") {
    condy::Runtime runtime(options);

    const int num_invokers = 20;
    std::vector<SetFinishInvoker> invokers(num_invokers);
    for (int i = 0; i < num_invokers; ++i) {
        runtime.schedule(&invokers[i]);
    }
    runtime.allow_exit();
    runtime.run();

    for (int i = 0; i < num_invokers; ++i) {
        REQUIRE(invokers[i].finished);
    }
}

TEST_CASE("test runtime - single thread schedule coroutine") {
    condy::Runtime runtime(options);

    bool finished = false;
    auto func = [&]() -> condy::Coro<void> {
        finished = true;
        co_return;
    };

    auto coro = func();
    auto h = coro.release();

    runtime.schedule(&h.promise());
    runtime.allow_exit();
    runtime.run();

    REQUIRE(finished);
}

TEST_CASE("test runtime - single thread schedule multiple coroutines") {
    condy::Runtime runtime(options);

    auto func = [](int &flag) -> condy::Coro<void> {
        flag = 1;
        co_return;
    };

    const int num_coros = 20;
    std::vector<int> finished_flags(num_coros, 0);
    std::vector<condy::Coro<void>> coros;
    coros.reserve(num_coros);
    for (int i = 0; i < num_coros; ++i) {
        coros.emplace_back(func(finished_flags[i]));
    }

    for (int i = 0; i < num_coros; ++i) {
        auto h = coros[i].release();
        runtime.schedule(&h.promise());
    }

    runtime.allow_exit();
    runtime.run();

    for (int i = 0; i < num_coros; ++i) {
        REQUIRE(finished_flags[i]);
    }
}

TEST_CASE("test runtime - single thread schedule coroutines with operation") {
    condy::Runtime runtime(options);

    auto func = [](int &flag) -> condy::Coro<void> {
        co_await condy::detail::make_op_awaiter(io_uring_prep_nop);
        flag = 1;
    };

    const int num_coros = 10;
    std::vector<int> finished_flags(num_coros, 0);
    std::vector<condy::Coro<void>> coros;
    coros.reserve(num_coros);
    for (int i = 0; i < num_coros; ++i) {
        coros.emplace_back(func(finished_flags[i]));
    }

    for (int i = 0; i < num_coros; ++i) {
        auto h = coros[i].release();
        runtime.schedule(&h.promise());
    }

    runtime.allow_exit();
    runtime.run();

    for (int i = 0; i < num_coros; ++i) {
        REQUIRE(finished_flags[i]);
    }
}

TEST_CASE("test runtime - single thread schedule coroutines with parallel "
          "operation") {
    using condy::operators::operator>>;
    condy::Runtime runtime(options);

    auto func = [](int &flag) -> condy::Coro<void> {
        co_await (condy::detail::make_op_awaiter(io_uring_prep_nop) >>
                  condy::detail::make_op_awaiter(io_uring_prep_nop));
        flag = 1;
    };

    const int num_coros = 10;
    std::vector<int> finished_flags(num_coros, 0);
    std::vector<condy::Coro<void>> coros;
    coros.reserve(num_coros);
    for (int i = 0; i < num_coros; ++i) {
        coros.emplace_back(func(finished_flags[i]));
    }

    for (int i = 0; i < num_coros; ++i) {
        auto h = coros[i].release();
        runtime.schedule(&h.promise());
    }

    runtime.allow_exit();
    runtime.run();

    for (int i = 0; i < num_coros; ++i) {
        REQUIRE(finished_flags[i]);
    }
}

TEST_CASE("test runtime - single thread schedule coroutine with cancel") {
    using condy::operators::operator||;

    condy::Runtime runtime(options);

    bool finished = false;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60ll * 60ll,
            .tv_nsec = 0,
        };
        auto r = co_await (
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0) ||
            condy::detail::make_op_awaiter(io_uring_prep_nop));
        REQUIRE(r.index() == 1); // nop path
        finished = true;
        co_return;
    };
    auto coro = func();
    auto h = coro.release();
    runtime.schedule(&h.promise());
    runtime.allow_exit();
    runtime.run();

    REQUIRE(finished);
}

TEST_CASE("test runtime - allow_exit from other runtime") {
    condy::Runtime runtime1(options);
    condy::Runtime runtime2(options);

    std::atomic_bool r1_started = false;
    auto func1 = [&]() -> condy::Coro<void> {
        r1_started = true;
        r1_started.notify_one();
        co_return;
    };

    condy::co_spawn(runtime1, func1()).detach();

    std::thread t1([&]() { runtime1.run(); });

    r1_started.wait(false);

    auto func2 = [&]() -> condy::Coro<void> {
        runtime1.allow_exit();
        co_return;
    };

    condy::co_spawn(runtime2, func2()).detach();
    runtime2.allow_exit();
    runtime2.run();

    t1.join();
}

TEST_CASE("test runtime - allow_exit from other thread") {
    condy::Runtime runtime(options);

    std::atomic_bool r1_started = false;
    auto func1 = [&]() -> condy::Coro<void> {
        r1_started = true;
        r1_started.notify_one();
        co_return;
    };

    condy::co_spawn(runtime, func1()).detach();

    std::thread t1([&]() { runtime.run(); });

    r1_started.wait(false);

    runtime.allow_exit();

    t1.join();
}