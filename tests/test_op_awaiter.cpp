#include "condy/buffers.hpp"
#include <condy/awaiter_operations.hpp>
#include <condy/awaiters.hpp>
#include <condy/coro.hpp>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <liburing.h>

namespace {

void event_loop(size_t &unfinished) {
    auto *ring = condy::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            if (io_uring_cqe_get_data(cqe) == condy::MagicData::IGNORE) {
                return;
            }
            auto handle_ptr = static_cast<condy::OpFinishHandle *>(
                io_uring_cqe_get_data(cqe));
            handle_ptr->set_result(cqe->res, cqe->flags);
            (*handle_ptr)();
        });
    }
}

// Just placeholder
condy::Runtime runtime;

} // namespace

TEST_CASE("test op_awaiter - basic routine") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring, &runtime);

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

TEST_CASE("test op_awaiter - multiple ops") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring, &runtime);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::make_op_awaiter(io_uring_prep_nop);
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

TEST_CASE("test op_awaiter - concurrent op") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring, &runtime);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto awaiter1 = condy::make_op_awaiter(io_uring_prep_nop);
        auto awaiter2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2] = co_await condy::WaitAllAwaiter<decltype(awaiter1),
                                                       decltype(awaiter2)>(
            std::move(awaiter1), std::move(awaiter2));
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
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

TEST_CASE("test op_awaiter - cancel op") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring, &runtime);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto awaiter1 =
            condy::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto awaiter2 = condy::make_op_awaiter(io_uring_prep_nop);
        auto awaiter =
            condy::WaitOneAwaiter<decltype(awaiter1), decltype(awaiter2)>(
                std::move(awaiter1), std::move(awaiter2));
        auto r = co_await awaiter;
        REQUIRE(r.index() == 1);
        REQUIRE(std::get<1>(r) == 0);
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

namespace {

void mock_multishot_event_loop(size_t &unfinished) {
    auto *ring = condy::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            if (io_uring_cqe_get_data(cqe) == condy::MagicData::IGNORE) {
                return;
            }
            auto handle_ptr = static_cast<condy::OpFinishHandle *>(
                io_uring_cqe_get_data(cqe));
            handle_ptr->set_result(42, 0);
            handle_ptr->multishot();
            handle_ptr->set_result(cqe->res, cqe->flags);
            (*handle_ptr)();
        });
    }
}

} // namespace

TEST_CASE("test op_awaiter - multishot op") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring, &runtime);

    bool handle_called = false;
    auto handle_multishot = [&](int res) -> condy::Coro<void> {
        REQUIRE(res == 42);
        handle_called = true;
        co_return;
    };

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::make_multishot_op_awaiter(
            [&](int res) {
                auto coro = handle_multishot(res);
                coro.release().resume();
            },
            io_uring_prep_nop);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    coro.release().resume();
    REQUIRE(unfinished == 1);

    mock_multishot_event_loop(unfinished);
    REQUIRE(unfinished == 0);
    REQUIRE(handle_called == true);

    context.reset();
}

TEST_CASE("test op_awaiter - select buffer op") {
    condy::Ring ring;
    io_uring_params params{};
    std::memset(&params, 0, sizeof(params));
    ring.init(8, &params);
    auto &context = condy::Context::current();
    context.init(&ring, &runtime);

    condy::detail::ProvidedBufferPoolImplPtr buffers_impl =
        std::make_shared<condy::detail::ProvidedBufferPoolImpl>(ring.ring(), 0,
                                                                4, 32, 0);

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    ::write(pipefd[1], "test", 4);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto [res, buf] = co_await condy::make_select_buffer_op_awaiter(
            buffers_impl, io_uring_prep_read, pipefd[0], nullptr, 0, 0);
        REQUIRE(res >= 0);
        REQUIRE(buf.size() == 32);
        REQUIRE(std::memcmp(buf.data(), "test", 4) == 0);
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