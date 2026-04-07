#include "condy/sender_operations.hpp"
#include "condy/sync_wait.hpp"
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