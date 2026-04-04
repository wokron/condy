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
        auto [order, r] = co_await condy::as_awaiter(condy::when_all_senders(
            condy::detail::make_op_sender(io_uring_prep_nop),
            condy::detail::make_op_sender(io_uring_prep_nop)));
        REQUIRE(std::get<0>(r) == 0);
        REQUIRE(std::get<1>(r) == 0);
    };
    condy::sync_wait(f());
}