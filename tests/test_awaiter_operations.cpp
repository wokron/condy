#include "condy/sync_wait.hpp"
#include <cerrno>
#include <condy/awaiter_operations.hpp>
#include <condy/awaiters.hpp>
#include <condy/coro.hpp>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <stdexcept>

using namespace condy::operators;

TEST_CASE("test awaiter_operations - test make_op_awaiter") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::detail::make_op_awaiter(io_uring_prep_nop);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test when_all") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2, r3] = co_await condy::when_all(aw1, aw2, aw3);
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
        REQUIRE(r3 == 0);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test when_any") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto aw1 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw3 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto r = co_await condy::when_any(aw1, aw2, aw3);
        REQUIRE(r.index() == 1);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test ranged when_all") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        std::vector<decltype(aw1)> awaiters;
        awaiters.emplace_back(aw1);
        awaiters.emplace_back(aw2);
        awaiters.emplace_back(aw3);
        auto r = co_await condy::when_all(std::move(awaiters));
        REQUIRE(r.size() == 3);
        REQUIRE(r == std::vector<int>{0, 0, 0});
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test ranged when_any") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts1{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        __kernel_timespec ts2{
            .tv_sec = 0,
            .tv_nsec = 100,
        };
        auto aw1 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts1, 0, 0);
        auto aw2 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts2, 0, 0);
        auto aw3 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts1, 0, 0);
        std::vector<decltype(aw1)> awaiters;
        awaiters.emplace_back(aw1);
        awaiters.emplace_back(aw2);
        awaiters.emplace_back(aw3);
        auto [idx, r] = co_await condy::when_any(std::move(awaiters));
        REQUIRE(idx == 1);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test &&") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2, r3] = co_await (aw1 && aw2 && aw3);
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
        REQUIRE(r3 == 0);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test ||") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto aw1 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw3 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto r = co_await (aw1 || aw2 || aw3);
        REQUIRE(r.index() == 1);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - mixed && and ||") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto aw1 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw4 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto [r1, r2] = co_await ((aw1 || aw2) && (aw3 || aw4));
        REQUIRE(r1.index() == 1);
        REQUIRE(r2.index() == 0);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - ranged awaiter push") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        std::vector<decltype(aw1)> awaiters = {aw1, aw2, aw3};
        auto awaiter = condy::when_all(std::move(awaiters));
        auto r = co_await awaiter;
        REQUIRE(r.size() == 3);
        REQUIRE(r == std::vector<int>{0, 0, 0});
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test link") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 0,
            .tv_nsec = 100,
        };
        auto aw1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw2 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw3 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2, r3] = co_await condy::link(aw1, aw2, aw3);
        REQUIRE(r1 == 0);
        REQUIRE(r2 == -ETIME);
        REQUIRE(r3 == -ECANCELED);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test >>") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 0,
            .tv_nsec = 100,
        };
        auto aw1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw2 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw3 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2, r3] = co_await (aw1 >> aw2 >> aw3);
        REQUIRE(r1 == 0);
        REQUIRE(r2 == -ETIME);
        REQUIRE(r3 == -ECANCELED);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test drain") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw = condy::detail::make_op_awaiter(io_uring_prep_nop);
        co_await condy::drain(aw);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test drain with when_all") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        co_await (aw1 && aw2 && condy::drain(aw3));
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test awaiter_operations - test parallel all") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto [order, results] =
            co_await condy::parallel<condy::ParallelAllAwaiter>(aw1, aw2, aw3);
        auto [r1, r2, r3] = results;
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
        REQUIRE(r3 == 0);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}