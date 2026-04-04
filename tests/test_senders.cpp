#include "condy/cqe_handler.hpp"
#include "condy/senders.hpp"
#include <doctest/doctest.h>
#include <liburing.h>
#include "condy/runtime.hpp"
#include "condy/sync_wait.hpp"


TEST_CASE("test senders - basic") {
    auto f = []() -> condy::Coro<void> {
        auto r = co_await condy::as_awaiter(condy::detail::make_op_sender(io_uring_prep_nop));
        REQUIRE(r == 0);
    };
    condy::sync_wait(f());
}