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