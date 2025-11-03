#include "condy/finish_handles.hpp"
#include <cerrno>
#include <condy/awaiter_operations.hpp>
#include <condy/awaiters.hpp>
#include <condy/coro.hpp>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <liburing.h>

using namespace condy::operators;

namespace {

void event_loop(size_t &unfinished) {
    auto *ring = condy::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            auto handle_ptr = static_cast<condy::OpFinishHandle *>(
                io_uring_cqe_get_data(cqe));
            handle_ptr->set_result(cqe->res);
            (*handle_ptr)();
        });
    }
}

} // namespace

TEST_CASE("test awaiter_operations - test make_op_awaiter") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::make_op_awaiter(io_uring_prep_nop);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.reset();
}

TEST_CASE("test awaiter_operations - test make_all_awaiter") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2, r3] = co_await condy::make_all_awaiter(
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

    context.reset();
}

TEST_CASE("test awaiter_operations - test make_one_awaiter") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto aw1 = condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto r = co_await condy::make_one_awaiter(
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

    context.reset();
}

TEST_CASE("test awaiter_operations - test make_ranged_all_awaiter") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_nop);
        std::vector<decltype(aw1)> awaiters;
        awaiters.emplace_back(std::move(aw1));
        awaiters.emplace_back(std::move(aw2));
        awaiters.emplace_back(std::move(aw3));
        auto r = co_await condy::make_ranged_all_awaiter(std::move(awaiters));
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

    context.reset();
}

TEST_CASE("test awaiter_operations - test make_ranged_one_awaiter") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

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
        auto aw1 = condy::make_op_awaiter(io_uring_prep_timeout, &ts1, 0, 0);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_timeout, &ts2, 0, 0);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_timeout, &ts1, 0, 0);
        std::vector<decltype(aw1)> awaiters;
        awaiters.emplace_back(std::move(aw1));
        awaiters.emplace_back(std::move(aw2));
        awaiters.emplace_back(std::move(aw3));
        auto [idx, r] =
            co_await condy::make_ranged_one_awaiter(std::move(awaiters));
        REQUIRE(idx == 1);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.reset();
}

TEST_CASE("test awaiter_operations - test &&") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_nop);
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

    context.reset();
}

TEST_CASE("test awaiter_operations - test ||") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto aw1 = condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
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

    context.reset();
}

TEST_CASE("test awaiter_operations - mixed && and ||") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto aw1 = condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw4 = condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
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

    context.reset();
}

TEST_CASE("test awaiter_operations - ranged +=") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_nop);
        auto awaiter =
            condy::make_ranged_all_awaiter(std::vector<decltype(aw1)>{});
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

    context.reset();
}

TEST_CASE("test awaiter_operations - test make_link_awaiter") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 0,
            .tv_nsec = 100,
        };
        auto aw1 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2, r3] = co_await condy::make_link_awaiter(
            std::move(aw1), std::move(aw2), std::move(aw3));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == -ETIME);
        REQUIRE(r3 == -ECANCELED);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.reset();
}

TEST_CASE("test awaiter_operations - test >>") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 0,
            .tv_nsec = 100,
        };
        auto aw1 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2, r3] =
            co_await (std::move(aw1) >> std::move(aw2) >> std::move(aw3));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == -ETIME);
        REQUIRE(r3 == -ECANCELED);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.reset();
}

TEST_CASE("test awaiter_operations - test make_drained_op_awaiter") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw = condy::make_op_awaiter(io_uring_prep_nop);
        co_await condy::make_drained_op_awaiter(std::move(aw));
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.reset();
}

TEST_CASE("test awaiter_operations - test ~") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_nop);
        co_await (std::move(aw1) && std::move(aw2) && ~std::move(aw3));
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    event_loop(unfinished);
    REQUIRE(unfinished == 0);

    context.reset();
}

TEST_CASE("test awaiter_operations - test make_parallel_awaiter") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto aw1 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto aw3 = condy::make_op_awaiter(io_uring_prep_nop);
        auto [order, results] = co_await condy::make_parallel_awaiter<
            condy::WaitAllCancelCondition>(std::move(aw1), std::move(aw2),
                                           std::move(aw3));
        auto [r1, r2, r3] = results;
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

    context.reset();
}