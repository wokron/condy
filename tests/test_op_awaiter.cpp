#include "condy/finish_handles.hpp"
#include "condy/provided_buffers.hpp"
#include <condy/awaiter_operations.hpp>
#include <condy/awaiters.hpp>
#include <condy/coro.hpp>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>

namespace {

void event_loop(size_t &unfinished) {
    auto *ring = condy::detail::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            auto [data, type] = condy::decode_work(io_uring_cqe_get_data(cqe));
            if (type == condy::WorkType::Ignore) {
                return;
            }
            auto handle_ptr = static_cast<condy::OpFinishHandleBase *>(data);
            handle_ptr->handle_cqe(cqe);
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
    auto &context = condy::detail::Context::current();
    context.init(&ring, &runtime);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::detail::make_op_awaiter(io_uring_prep_nop);
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
    auto &context = condy::detail::Context::current();
    context.init(&ring, &runtime);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::detail::make_op_awaiter(io_uring_prep_nop);
        co_await condy::detail::make_op_awaiter(io_uring_prep_nop);
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
    auto &context = condy::detail::Context::current();
    context.init(&ring, &runtime);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto awaiter1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto awaiter2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2] = co_await condy::WhenAllAwaiter<decltype(awaiter1),
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
    auto &context = condy::detail::Context::current();
    context.init(&ring, &runtime);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        __kernel_timespec ts{
            .tv_sec = 60,
            .tv_nsec = 0,
        };
        auto awaiter1 =
            condy::detail::make_op_awaiter(io_uring_prep_timeout, &ts, 0, 0);
        auto awaiter2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto awaiter =
            condy::WhenAnyAwaiter<decltype(awaiter1), decltype(awaiter2)>(
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
    auto *ring = condy::detail::Context::current().ring();
    while (unfinished > 0) {
        ring->submit();
        ring->reap_completions([&](io_uring_cqe *cqe) {
            auto [data, type] = condy::decode_work(io_uring_cqe_get_data(cqe));
            if (type == condy::WorkType::Ignore) {
                return;
            }
            auto handle_ptr = static_cast<condy::OpFinishHandleBase *>(data);
            // Mock Multishot
            io_uring_cqe mock_cqe = *cqe;
            mock_cqe.res = 42;
            mock_cqe.flags |= IORING_CQE_F_MORE;
            handle_ptr->handle_cqe(&mock_cqe);
            handle_ptr->handle_cqe(cqe);
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
    auto &context = condy::detail::Context::current();
    context.init(&ring, &runtime);

    bool handle_called = false;
    auto handle_multishot = [&](int res) -> condy::Coro<void> {
        REQUIRE(res == 42);
        handle_called = true;
        co_return;
    };

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::detail::make_multishot_op_awaiter(
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
    auto &context = condy::detail::Context::current();
    context.init(&ring, &runtime);

    {
        condy::ProvidedBufferPool pool(16, 32);

        int pipefd[2];
        REQUIRE(pipe(pipefd) == 0);

        ssize_t r = ::write(pipefd[1], "test", 4);
        REQUIRE(r == 4);

        size_t unfinished = 1;
        auto func = [&]() -> condy::Coro<void> {
            auto [res, buf] =
                co_await condy::detail::make_select_buffer_op_awaiter(
                    &pool, io_uring_prep_read, pipefd[0], nullptr, 0, 0);
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
    }

    context.reset();
}