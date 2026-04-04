#include "condy/cqe_handler.hpp"
#include "condy/runtime.hpp"
#include "condy/senders.hpp"
#include "condy/sync_wait.hpp"
#include <doctest/doctest.h>
#include <liburing.h>

TEST_CASE("test senders - basic") {
    auto f = []() -> condy::Coro<void> {
        auto r = co_await condy::as_awaiter(
            condy::detail::make_op_sender(io_uring_prep_nop));
        REQUIRE(r == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - when_all") {
    auto f = []() -> condy::Coro<void> {
        auto [r1, r2] = co_await condy::as_awaiter(condy::when_all_senders(
            condy::detail::make_op_sender(io_uring_prep_nop),
            condy::detail::make_op_sender(io_uring_prep_nop)));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
    };
    condy::sync_wait(f());
}

TEST_CASE("test senders - when_any") {
    auto f = []() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto r = co_await condy::as_awaiter(
            condy::when_any_senders(condy::detail::make_op_sender(
                io_uring_prep_timeout, &ts, 0, 0),
            condy::detail::make_op_sender(io_uring_prep_nop)));
        REQUIRE(r.index() == 1);
        REQUIRE(std::get<1>(r) == 0);
    };
    condy::sync_wait(f());
}