#include "condy/awaiter_operations.hpp"
#include "condy/context.hpp"
#include "condy/coro.hpp"
#include "condy/ring.hpp"
#include "condy/sync_wait.hpp"
#include <doctest/doctest.h>

TEST_CASE("test fd_table - init") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::Context::current().ring()->fd_table();
        fd_table.init(4);
        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test fd_table - register/unregister fd") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::Context::current().ring()->fd_table();
        fd_table.init(4);

        int pipes[2];
        int ret = pipe(pipes);
        REQUIRE(ret == 0);

        fd_table.update_files(0, pipes, 2);

        close(pipes[0]);
        close(pipes[1]);

        pipes[0] = -1;
        pipes[1] = -1;

        fd_table.update_files(0, pipes, 2);

        co_return;
    };

    condy::sync_wait(func());
}

TEST_CASE("test fd_table - use fixed fd") {
    auto func = []() -> condy::Coro<void> {
        auto &fd_table = condy::Context::current().ring()->fd_table();
        fd_table.init(4);

        int pipes[2];
        int ret = pipe(pipes);
        REQUIRE(ret == 0);

        fd_table.update_files(0, pipes, 2);

        char write_buf[] = "hello";
        char read_buf[sizeof(write_buf)] = {0};

        auto write_op = condy::make_op_awaiter(io_uring_prep_write, 1,
                                               write_buf, sizeof(write_buf), 0);
        write_op.add_flags(IOSQE_FIXED_FILE);
        int write_res = co_await write_op;
        REQUIRE(write_res == sizeof(write_buf));

        auto read_op = condy::make_op_awaiter(io_uring_prep_read, 0, read_buf,
                                              sizeof(read_buf), 0);
        read_op.add_flags(IOSQE_FIXED_FILE);
        int read_res = co_await read_op;
        REQUIRE(read_res == sizeof(read_buf));
        REQUIRE(std::memcmp(write_buf, read_buf, sizeof(write_buf)) == 0);
        co_return;
    };

    condy::sync_wait(func());
}