#include "condy/awaiter_operations.hpp"
#include "condy/coro.hpp"
#include "condy/invoker.hpp"
#include "condy/runtime.hpp"
#include "condy/runtime_options.hpp"
#include <atomic>
#include <cstddef>
#include <doctest/doctest.h>
#include <unordered_set>

namespace {

struct SetFinishInvoker
    : public condy::InvokerAdapter<SetFinishInvoker, condy::WorkInvoker> {
    void operator()() { finished = true; }
    bool finished = false;
};

condy::SingleThreadOptions options =
    condy::SingleThreadOptions().sq_size(8).cq_size(16);

} // namespace

TEST_CASE("test runtime - single thread no-op") {
    condy::SingleThreadRuntime runtime(options);
    runtime.done();
    runtime.wait(); // Should exit immediately
}

TEST_CASE("test runtime - single thread schedule no-op") {
    condy::SingleThreadRuntime runtime(options);

    SetFinishInvoker invoker;
    runtime.schedule(&invoker);
    runtime.done();
    runtime.wait();

    REQUIRE(invoker.finished);
}

TEST_CASE("test runtime - single thread schedule multiple no-op") {
    condy::SingleThreadRuntime runtime(options);

    const int num_invokers = 20;
    std::vector<SetFinishInvoker> invokers(num_invokers);
    for (int i = 0; i < num_invokers; ++i) {
        runtime.schedule(&invokers[i]);
    }
    runtime.done();
    runtime.wait();

    for (int i = 0; i < num_invokers; ++i) {
        REQUIRE(invokers[i].finished);
    }
}

TEST_CASE("test runtime - single thread schedule coroutine") {
    condy::SingleThreadRuntime runtime(options);

    bool finished = false;
    auto func = [&]() -> condy::Coro<void> {
        finished = true;
        co_return;
    };

    auto coro = func();
    auto h = coro.release();

    runtime.schedule(&h.promise());
    runtime.done();
    runtime.wait();

    REQUIRE(finished);
}

TEST_CASE("test runtime - single thread schedule multiple coroutines") {
    condy::SingleThreadRuntime runtime(options);

    auto func = [](int &flag) -> condy::Coro<void> {
        flag = 1;
        co_return;
    };

    const int num_coros = 20;
    std::vector<int> finished_flags(num_coros, 0);
    std::vector<condy::Coro<void>> coros;
    for (int i = 0; i < num_coros; ++i) {
        coros.emplace_back(func(finished_flags[i]));
    }

    for (int i = 0; i < num_coros; ++i) {
        auto h = coros[i].release();
        runtime.schedule(&h.promise());
    }

    runtime.done();
    runtime.wait();

    for (int i = 0; i < num_coros; ++i) {
        REQUIRE(finished_flags[i]);
    }
}

TEST_CASE("test runtime - single thread schedule coroutines with operation") {
    condy::SingleThreadRuntime runtime(options);

    auto func = [](int &flag) -> condy::Coro<void> {
        co_await condy::make_op_awaiter(io_uring_prep_nop);
        flag = 1;
    };

    const int num_coros = 10;
    std::vector<int> finished_flags(num_coros, 0);
    std::vector<condy::Coro<void>> coros;
    for (int i = 0; i < num_coros; ++i) {
        coros.emplace_back(func(finished_flags[i]));
    }

    for (int i = 0; i < num_coros; ++i) {
        auto h = coros[i].release();
        runtime.schedule(&h.promise());
    }

    runtime.done();
    runtime.wait();

    for (int i = 0; i < num_coros; ++i) {
        REQUIRE(finished_flags[i]);
    }
}

TEST_CASE("test runtime - single thread schedule coroutines with parallel "
          "operation") {
    using condy::operators::operator>>;
    condy::SingleThreadRuntime runtime(options);

    auto func = [](int &flag) -> condy::Coro<void> {
        co_await (condy::make_op_awaiter(io_uring_prep_nop) >>
                  condy::make_op_awaiter(io_uring_prep_nop));
        flag = 1;
    };

    const int num_coros = 10;
    std::vector<int> finished_flags(num_coros, 0);
    std::vector<condy::Coro<void>> coros;
    for (int i = 0; i < num_coros; ++i) {
        coros.emplace_back(func(finished_flags[i]));
    }

    for (int i = 0; i < num_coros; ++i) {
        auto h = coros[i].release();
        runtime.schedule(&h.promise());
    }

    runtime.done();
    runtime.wait();

    for (int i = 0; i < num_coros; ++i) {
        REQUIRE(finished_flags[i]);
    }
}

TEST_CASE("test runtime - single thread schedule coroutine with cancel") {
    using condy::operators::operator||;

    condy::SingleThreadRuntime runtime(options);

    bool finished = false;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60 * 60,
            .tv_nsec = 0,
        };
        auto r = co_await (
            condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0) ||
            condy::make_op_awaiter(io_uring_prep_nop));
        REQUIRE(r.index() == 1); // nop path
        finished = true;
        co_return;
    };
    auto coro = func();
    auto h = coro.release();
    runtime.schedule(&h.promise());
    runtime.done();
    runtime.wait();

    REQUIRE(finished);
}

TEST_CASE("test runtime - multi thread schedule no-op") {
    condy::MultiThreadRuntime runtime(8, 4);

    SetFinishInvoker invoker;
    runtime.schedule(&invoker);
    runtime.done();
    runtime.wait();

    REQUIRE(invoker.finished);
}

TEST_CASE("test runtime - multi thread schedule multiple no-op") {
    condy::MultiThreadRuntime runtime(8, 4);

    const int num_invokers = 20;
    std::vector<SetFinishInvoker> invokers(num_invokers);
    for (int i = 0; i < num_invokers; ++i) {
        runtime.schedule(&invokers[i]);
    }
    runtime.done();
    runtime.wait();

    for (int i = 0; i < num_invokers; ++i) {
        REQUIRE(invokers[i].finished);
    }
}

TEST_CASE("test runtime - multi thread schedule coroutine") {
    condy::MultiThreadRuntime runtime(8, 4);

    bool finished = false;
    auto func = [&]() -> condy::Coro<void> {
        finished = true;
        co_return;
    };

    auto coro = func();
    auto h = coro.release();

    runtime.schedule(&h.promise());
    runtime.done();
    runtime.wait();

    REQUIRE(finished);
}

TEST_CASE("test runtime - multi thread schedule multiple coroutines") {
    condy::MultiThreadRuntime runtime(8, 2);

    std::mutex mu;
    std::unordered_set<std::thread::id> thread_ids;

    auto func = [&](int &flag) -> condy::Coro<void> {
        std::lock_guard<std::mutex> lock(mu);
        thread_ids.insert(std::this_thread::get_id());
        flag = 1;
        co_return;
    };

    const int num_coros = 1000;
    std::vector<int> finished_flags(num_coros, 0);
    std::vector<condy::Coro<void>> coros;
    for (int i = 0; i < num_coros; ++i) {
        coros.emplace_back(func(finished_flags[i]));
    }

    for (int i = 0; i < num_coros; ++i) {
        auto h = coros[i].release();
        runtime.schedule(&h.promise());
    }

    runtime.done();
    runtime.wait();

    REQUIRE(std::all_of(finished_flags.begin(), finished_flags.end(),
                        [](int flag) { return flag == 1; }));

    REQUIRE(thread_ids.size() > 1); // Ensure multiple threads were used
}

TEST_CASE("test runtime - multi thread schedule coroutines with operation") {
    condy::MultiThreadRuntime runtime(8, 4);

    auto func = [](int &flag) -> condy::Coro<void> {
        co_await condy::make_op_awaiter(io_uring_prep_nop);
        flag = 1;
    };

    const int num_coros = 20;
    std::vector<int> finished_flags(num_coros, 0);
    std::vector<condy::Coro<void>> coros;
    for (int i = 0; i < num_coros; ++i) {
        coros.emplace_back(func(finished_flags[i]));
    }

    for (int i = 0; i < num_coros; ++i) {
        auto h = coros[i].release();
        runtime.schedule(&h.promise());
    }

    runtime.done();
    runtime.wait();

    for (int i = 0; i < num_coros; ++i) {
        REQUIRE(finished_flags[i]);
    }
}

TEST_CASE("test runtime - multi thread schedule coroutines with parallel "
          "operation") {
    using condy::operators::operator>>;
    condy::MultiThreadRuntime runtime(8, 4);

    auto func = [](int &flag) -> condy::Coro<void> {
        co_await (condy::make_op_awaiter(io_uring_prep_nop) >>
                  condy::make_op_awaiter(io_uring_prep_nop));
        flag = 1;
    };

    const int num_coros = 20;
    std::vector<int> finished_flags(num_coros, 0);
    std::vector<condy::Coro<void>> coros;
    for (int i = 0; i < num_coros; ++i) {
        coros.emplace_back(func(finished_flags[i]));
    }

    for (int i = 0; i < num_coros; ++i) {
        auto h = coros[i].release();
        runtime.schedule(&h.promise());
    }

    runtime.done();
    runtime.wait();

    for (int i = 0; i < num_coros; ++i) {
        REQUIRE(finished_flags[i]);
    }
}

TEST_CASE("test runtime - multi thread schedule coroutine with cancel") {
    using condy::operators::operator||;

    condy::MultiThreadRuntime runtime(8, 4);

    bool finished = false;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60 * 60,
            .tv_nsec = 0,
        };
        auto r = co_await (
            condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0) ||
            condy::make_op_awaiter(io_uring_prep_nop));
        REQUIRE(r.index() == 1); // nop path
        finished = true;
        co_return;
    };
    auto coro = func();
    auto h = coro.release();
    runtime.schedule(&h.promise());
    runtime.done();
    runtime.wait();

    REQUIRE(finished);
}

TEST_CASE(
    "test runtime - multi thread schedule high concurrency with cancelation") {
    condy::MultiThreadRuntime runtime(64, 8);

    const int num_ops = 100;

    auto func = [&](std::atomic_size_t &finished) -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 1,
            .tv_nsec = 0,
        };
        using Awaiter =
            decltype(condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0));
        std::vector<Awaiter> awaiters;
        std::vector<__kernel_timespec> ts_vec(num_ops);
        awaiters.reserve(num_ops);
        for (int i = 0; i < num_ops; ++i) {
            auto &ts = ts_vec[i];
            ts.tv_nsec += 1000 * (i % 10); // Stagger timeouts a bit
            awaiters.emplace_back(
                condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0));
        }
        auto one_awaiter = condy::make_ranged_one_awaiter(std::move(awaiters));
        auto [no, r] = co_await one_awaiter;
        REQUIRE(0 <= no);
        REQUIRE(no < num_ops);
        REQUIRE(r == -ETIME);
        finished++;
    };

    std::atomic_size_t finished = 0;

    const size_t num_coro = 1000;
    for (size_t i = 0; i < num_coro; i++) {
        auto coro = func(finished);
        auto h = coro.release();
        runtime.schedule(&h.promise());
    }

    runtime.done();
    runtime.wait();

    REQUIRE(finished == num_coro);
}