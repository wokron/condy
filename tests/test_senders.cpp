#include "condy/sender_operations.hpp"
#include "condy/senders.hpp"
#include "condy/sync_wait.hpp"
#include <cerrno>
#include <doctest/doctest.h>
#include <liburing.h>

TEST_CASE("test senders - basic") {
    auto f = []() -> condy::Coro<void> {
        auto r = co_await condy::detail::as_awaiter(
            condy::detail::make_op_sender(io_uring_prep_nop));
        REQUIRE(r == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - when_all") {
    auto f = []() -> condy::Coro<void> {
        auto [r1, r2] =
            co_await condy::detail::as_awaiter(condy::temp::when_all(
                condy::detail::make_op_sender(io_uring_prep_nop),
                condy::detail::make_op_sender(io_uring_prep_nop)));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - parallel all") {
    auto f = []() -> condy::Coro<void> {
        auto [order, r] = co_await condy::detail::as_awaiter(
            condy::temp::parallel<condy::ParallelAllSender>(
                condy::detail::make_op_sender(io_uring_prep_nop),
                condy::detail::make_op_sender(io_uring_prep_nop)));
        REQUIRE(order[0] == 0);
        REQUIRE(order[1] == 1);
        REQUIRE(std::get<0>(r) == 0);
        REQUIRE(std::get<1>(r) == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - when_any") {
    auto f = []() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto r = co_await condy::detail::as_awaiter(condy::temp::when_any(
            condy::detail::make_op_sender(io_uring_prep_nop),
            condy::detail::make_op_sender(io_uring_prep_timeout, &ts, 0, 0)));
        REQUIRE(r.index() == 0);
        REQUIRE(std::get<0>(r) == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - parallel any") {
    auto f = []() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto [order, r] = co_await condy::detail::as_awaiter(
            condy::temp::parallel<condy::ParallelAnySender>(
                condy::detail::make_op_sender(io_uring_prep_nop),
                condy::detail::make_op_sender(io_uring_prep_timeout, &ts, 0, 0)));
        REQUIRE(order[0] == 0);
        REQUIRE(std::get<0>(r) == 0);
        REQUIRE(std::get<1>(r) == -ECANCELED);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - link") {
    auto f = []() -> condy::Coro<void> {
        auto [r1, r2] = co_await condy::detail::as_awaiter(condy::temp::link(
            condy::detail::make_op_sender(io_uring_prep_nop),
            condy::detail::make_op_sender(io_uring_prep_nop)));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - hard_link") {
    auto f = []() -> condy::Coro<void> {
        auto [r1, r2] =
            co_await condy::detail::as_awaiter(condy::temp::hard_link(
                condy::detail::make_op_sender(io_uring_prep_nop),
                condy::detail::make_op_sender(io_uring_prep_nop)));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - flags") {
    auto f = []() -> condy::Coro<void> {
        auto r = co_await condy::detail::as_awaiter(condy::temp::always_async(
            condy::detail::make_op_sender(io_uring_prep_nop)));
        REQUIRE(r == 0);
    };
    condy::sync_wait(f());
}