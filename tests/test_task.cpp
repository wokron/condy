#include "condy/awaiter_operations.hpp"
#include "condy/coro.hpp"
#include <condy/task.hpp>
#include <cstddef>
#include <doctest/doctest.h>

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
            handle_ptr->finish(cqe->res);
        }

        io_uring_cqe_seen(ring, cqe);
    }
}

} // namespace

TEST_CASE("test task - launch multiple tasks") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;

    auto sub_func = [&](int v, int &r) -> condy::Coro<void> {
        co_await condy::build_op_awaiter(io_uring_prep_nop);
        r = v;
    };

    auto func = [&]() -> condy::Coro<void> {
        int r1 = 0, r2 = 0, r3 = 0;
        auto t1 = condy::co_spawn(sub_func(1, r1));
        auto t2 = condy::co_spawn(sub_func(2, r2));
        auto t3 = condy::co_spawn(sub_func(3, r3));
        co_await std::move(t3);
        REQUIRE(r3 == 3);
        co_await std::move(t2);
        REQUIRE(r2 == 2);
        co_await std::move(t1);
        REQUIRE(r1 == 1);
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
