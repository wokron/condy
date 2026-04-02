#include "condy/finish_handles.hpp"
#include "condy/provided_buffers.hpp"
#include "condy/sync_wait.hpp"
#include <condy/awaiter_operations.hpp>
#include <condy/awaiters.hpp>
#include <condy/coro.hpp>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>

TEST_CASE("test op_awaiter - basic routine") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::detail::make_op_awaiter(io_uring_prep_nop);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test op_awaiter - multiple ops") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        co_await condy::detail::make_op_awaiter(io_uring_prep_nop);
        co_await condy::detail::make_op_awaiter(io_uring_prep_nop);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test op_awaiter - concurrent op") {
    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        auto awaiter1 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto awaiter2 = condy::detail::make_op_awaiter(io_uring_prep_nop);
        auto [r1, r2] = co_await condy::WhenAllAwaiter<decltype(awaiter1),
                                                       decltype(awaiter2)>(
            awaiter1, awaiter2);
        REQUIRE(r1 == 0);
        REQUIRE(r2 == 0);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test op_awaiter - cancel op") {
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
                awaiter1, awaiter2);
        auto r = co_await awaiter;
        REQUIRE(r.index() == 1);
        REQUIRE(std::get<1>(r) == 0);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);
}

TEST_CASE("test op_awaiter - select buffer op") {
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    ssize_t r = ::write(pipefd[1], "test", 4);
    REQUIRE(r == 4);

    size_t unfinished = 1;
    auto func = [&]() -> condy::Coro<void> {
        condy::ProvidedBufferPool pool(16, 32);
        auto [res, buf] = co_await condy::detail::make_select_buffer_op_awaiter(
            &pool, io_uring_prep_read, pipefd[0], nullptr, 0, 0);
        REQUIRE(res >= 0);
        REQUIRE(buf.size() == 32);
        REQUIRE(std::memcmp(buf.data(), "test", 4) == 0);
        --unfinished;
    };

    auto coro = func();
    REQUIRE(unfinished == 1);

    condy::sync_wait(std::move(coro));
    REQUIRE(unfinished == 0);

    close(pipefd[0]);
    close(pipefd[1]);
}