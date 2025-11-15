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
    void invoke() { finished = true; }
    bool finished = false;
};

condy::RuntimeOptions options =
    condy::RuntimeOptions().sq_size(8).cq_size(16);

} // namespace

TEST_CASE("test runtime - single thread no-op") {
    condy::Runtime runtime(options);
    runtime.done();
    runtime.wait(); // Should exit immediately
}

TEST_CASE("test runtime - single thread schedule no-op") {
    condy::Runtime runtime(options);

    SetFinishInvoker invoker;
    runtime.schedule(&invoker);
    runtime.done();
    runtime.wait();

    REQUIRE(invoker.finished);
}

TEST_CASE("test runtime - single thread schedule multiple no-op") {
    condy::Runtime runtime(options);

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
    condy::Runtime runtime(options);

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
    condy::Runtime runtime(options);

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
    condy::Runtime runtime(options);

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
    condy::Runtime runtime(options);

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

    condy::Runtime runtime(options);

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
