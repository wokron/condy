#include "condy/async_operations.hpp"
#include "condy/awaiter_operations.hpp"
#include "condy/channel.hpp"
#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/ring.hpp"
#include "condy/runtime.hpp"
#include "condy/sync_wait.hpp"
#include <cerrno>
#include <doctest/doctest.h>
#include <stdexcept>
#include <variant>

TEST_CASE("test fd_table - init") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::detail::Context::current().ring()->fd_table();
        fd_table.init(4);
        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test fd_table - register/unregister fd") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::detail::Context::current().ring()->fd_table();
        fd_table.init(4);

        int pipes[2];
        int ret = pipe(pipes);
        REQUIRE(ret == 0);

        int r = fd_table.update(0, pipes, 2);
        REQUIRE(r == 2);

        close(pipes[0]);
        close(pipes[1]);

        pipes[0] = -1;
        pipes[1] = -1;

        r = fd_table.update(0, pipes, 2);
        REQUIRE(r == 2);

        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test fd_table - init with fd array") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::detail::Context::current().ring()->fd_table();

        int pipes[2];
        int ret = pipe(pipes);
        REQUIRE(ret == 0);

        int r = fd_table.init(pipes, 2);
        REQUIRE(r == 0);

        close(pipes[0]);
        close(pipes[1]);

        pipes[0] = -1;
        pipes[1] = -1;

        r = fd_table.update(0, pipes, 2);
        REQUIRE(r == 2);

        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test fd_table - use fixed fd") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::detail::Context::current().ring()->fd_table();
        fd_table.init(4);

        int pipes[2];
        int ret = pipe(pipes);
        REQUIRE(ret == 0);

        fd_table.update(0, pipes, 2);

        char write_buf[] = "hello";
        char read_buf[sizeof(write_buf)] = {0};

        auto write_op = condy::detail::make_op_awaiter(
            io_uring_prep_write, 1, write_buf, sizeof(write_buf), 0);
        int write_res = co_await condy::flag<IOSQE_FIXED_FILE>(write_op);
        REQUIRE(write_res == sizeof(write_buf));

        auto read_op = condy::detail::make_op_awaiter(
            io_uring_prep_read, 0, read_buf, sizeof(read_buf), 0);
        int read_res = co_await condy::flag<IOSQE_FIXED_FILE>(read_op);
        REQUIRE(read_res == sizeof(read_buf));
        REQUIRE(std::memcmp(write_buf, read_buf, sizeof(write_buf)) == 0);
        co_return;
    };

    condy::sync_wait(func());
}
