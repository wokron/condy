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
            handle_ptr->finish(cqe->res);
        }

        io_uring_cqe_seen(ring, cqe);
    }
}

} // namespace

TEST_CASE("test awaiter_operations - test build_op_awaiter") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro {
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

TEST_CASE("test awaiter_operations - test build_all_awaiter") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro {
        auto aw1 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::build_op_awaiter(io_uring_prep_nop);
        auto [r1, r2, r3] = co_await condy::build_all_awaiter(
            std::move(aw1), std::move(aw2), std::move(aw3));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
        REQUIRE(r3 == 0);
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

TEST_CASE("test awaiter_operations - test build_one_awaiter") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto aw1 = condy::build_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw2 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::build_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto r = co_await condy::build_one_awaiter(
            std::move(aw1), std::move(aw2), std::move(aw3));
        REQUIRE(r.index() == 1);
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

TEST_CASE("test awaiter_operations - test build_ranged_all_awaiter") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro {
        auto aw1 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::build_op_awaiter(io_uring_prep_nop);
        std::vector<decltype(aw1)> awaiters;
        awaiters.emplace_back(std::move(aw1));
        awaiters.emplace_back(std::move(aw2));
        awaiters.emplace_back(std::move(aw3));
        auto r = co_await condy::build_ranged_all_awaiter(std::move(awaiters));
        REQUIRE(r.size() == 3);
        REQUIRE(r == std::vector<int>{0, 0, 0});
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

TEST_CASE("test awaiter_operations - test build_ranged_one_awaiter") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro {
        __kernel_timespec ts1{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        __kernel_timespec ts2{
            .tv_sec = 0,
            .tv_nsec = 100,
        };
        auto aw1 = condy::build_op_awaiter(io_uring_prep_timeout, &ts1, 0, 0);
        auto aw2 = condy::build_op_awaiter(io_uring_prep_timeout, &ts2, 0, 0);
        auto aw3 = condy::build_op_awaiter(io_uring_prep_timeout, &ts1, 0, 0);
        std::vector<decltype(aw1)> awaiters;
        awaiters.emplace_back(std::move(aw1));
        awaiters.emplace_back(std::move(aw2));
        awaiters.emplace_back(std::move(aw3));
        auto [idx, r] =
            co_await condy::build_ranged_one_awaiter(std::move(awaiters));
        REQUIRE(idx == 1);
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

TEST_CASE("test awaiter_operations - test &&") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro {
        auto aw1 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::build_op_awaiter(io_uring_prep_nop);
        auto [r1, r2, r3] =
            co_await (std::move(aw1) && std::move(aw2) && std::move(aw3));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
        REQUIRE(r3 == 0);
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

TEST_CASE("test awaiter_operations - test ||") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto aw1 = condy::build_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw2 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::build_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto r = co_await (std::move(aw1) || std::move(aw2) || std::move(aw3));
        REQUIRE(r.index() == 1);
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

TEST_CASE("test awaiter_operations - mixed && and ||") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto aw1 = condy::build_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw2 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw4 = condy::build_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto [r1, r2] = co_await ((std::move(aw1) || std::move(aw2)) &&
                                  (std::move(aw3) || std::move(aw4)));
        REQUIRE(r1.index() == 1);
        REQUIRE(r2.index() == 0);
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

TEST_CASE("test awaiter_operations - ranged +=") {
    auto &context = condy::Context::current();
    context.init({.io_uring_entries = 8});

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro {
        auto aw1 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::build_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::build_op_awaiter(io_uring_prep_nop);
        auto awaiter =
            condy::build_ranged_all_awaiter(std::vector<decltype(aw1)>{});
        awaiter += std::move(aw1);
        awaiter += std::move(aw2);
        awaiter += std::move(aw3);
        auto r = co_await std::move(awaiter);
        REQUIRE(r.size() == 3);
        REQUIRE(r == std::vector<int>{0, 0, 0});
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