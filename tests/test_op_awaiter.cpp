#include <condy/awaiter_operations.hpp>
#include <condy/awaiters.hpp>
#include <condy/coro.hpp>
#include <cstddef>
#include <doctest/doctest.h>
#include <liburing.h>

namespace {

void event_loop(size_t &unfinished) {
    auto &context = condy::Context::current();
    auto ring = context.get_ring();
    while (unfinished > 0) {
        io_uring_submit_and_wait(ring, 1);

        io_uring_cqe *cqe;
        io_uring_peek_cqe(ring, &cqe);
        if (cqe == nullptr) {
            continue;
        }

        auto handle_ptr =
            static_cast<condy::OpFinishHandle *>(io_uring_cqe_get_data(cqe));
        if (handle_ptr) {
            handle_ptr->invoke(cqe->res);
        }

        io_uring_cqe_seen(ring, cqe);
    }
}

} // namespace

TEST_CASE("test op_awaiter - basic routine") {
    condy::SimpleStrategy strategy(8);
    auto &context = condy::Context::current();
    context.init(&strategy, nullptr, nullptr);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::build_op_awaiter(io_uring_prep_nop);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.destroy();
}

TEST_CASE("test op_awaiter - multiple ops") {
    condy::SimpleStrategy strategy(8);
    auto &context = condy::Context::current();
    context.init(&strategy, nullptr, nullptr);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::build_op_awaiter(io_uring_prep_nop);
        co_await condy::build_op_awaiter(io_uring_prep_nop);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.destroy();
}

TEST_CASE("test op_awaiter - concurrent op") {
    condy::SimpleStrategy strategy(8);
    auto &context = condy::Context::current();
    context.init(&strategy, nullptr, nullptr);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto awaiter1 = condy::build_op_awaiter(io_uring_prep_nop);
        auto awaiter2 = condy::build_op_awaiter(io_uring_prep_nop);
        auto [r1, r2] = co_await condy::WaitAllAwaiter<decltype(awaiter1),
                                                       decltype(awaiter2)>(
            std::move(awaiter1), std::move(awaiter2));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.destroy();
}

TEST_CASE("test op_awaiter - cancel op") {
    condy::SimpleStrategy strategy(8);
    auto &context = condy::Context::current();
    context.init(&strategy, nullptr, nullptr);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto awaiter1 =
            condy::build_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto awaiter2 = condy::build_op_awaiter(io_uring_prep_nop);
        auto awaiter =
            condy::WaitOneAwaiter<decltype(awaiter1), decltype(awaiter2)>(
                std::move(awaiter1), std::move(awaiter2));
        auto r = co_await awaiter;
        REQUIRE(r.index() == 1);
        REQUIRE(std::get<1>(r) == 0);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.destroy();
}